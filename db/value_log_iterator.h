// changes needed in main Rocks code:
// FileMetaData needs a unique ID number for each SST in the system.  Could be unique per column family if that's easier
// Manifest needs smallest file# referenced by the SST

//  Copyright (c) 2017-present, Toshiba Memory America, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#include <list>
#include <string>
#include <vector>
#include "options/cf_options.h"
#include "table/internal_iterator.h"
#include "db/column_family.h"
#include "db/compaction_iterator.h"
#include "db/value_log.h"



namespace rocksdb {

// Iterator class to go through the files for Active Recycling, in order
//
// This does essentially the same thing as the TwoLevelIterator, but without a LevelFilesBrief, and with the facility for
// determining when an input file is exhausted.  Rather than put switches
// into all the other iterator types, we just do everything here, in a single level.
class RecyclingIterator : public InternalIterator {
 public:
  explicit RecyclingIterator(Compaction *compaction_, VersionSet* versions_);

  virtual void SeekToFirst() override { file_index = -1; file_iterator.reset(); Next(); }  // set up to get the first record
  virtual void Next() override;
  virtual Slice key() const override { return file_iterator->key(); }
  virtual Slice value() const override { return file_iterator->value(); }
  // Return good status to a read past the end of keys.  Have to, because there is no benign EOF status
  virtual Status status() const override { return file_index<compaction->inputs()->size() ? file_iterator->status() : Status(); }
  virtual bool Valid() const override { return file_index<compaction->inputs()->size(); }
  size_t fileindex() { return file_index; }

  // The following are needed to meet the requirements of the InternalIterator:
  virtual void SeekToLast() override {};
  virtual void SeekForPrev(const Slice& target) override {};
  virtual void Seek(const Slice& target) override {};
  virtual void Prev() override {};
  
private:
  Compaction *compaction;  // the compaction we are working on.  Points to everything we need
  size_t file_index;  // index of the file the current kv came from
  std::unique_ptr<InternalIterator> file_iterator;  // pointer to iterator for the current file
  ReadOptions read_options;  // options we will use for reading tables
  const EnvOptions *env_options;  // env options for reading the database
};




// Iterator class to layer between the compaction_job loop and the compaction_iterator
// We read all the key/values for the compaction and buffer them, write indirect values to disk, and then
// return the possibly modified kvs one by one as the iterator result
class IndirectIterator {
public: 
  static const VLogRingRefFileno high_value = ((VLogRingRefFileno)-1)>>1;  // biggest positive value

  IndirectIterator(
   CompactionIterator* c_iter,   // the input iterator that feeds us kvs
   ColumnFamilyData* cfd,  // the column family we are working on
   const Compaction *compaction,   // variuos info for this compaction
   Slice *end,   // the last+1 key to include (i. e. end of open interval), or nullptr if not given
   bool use_indirects,   // if false, do not do any indirect processing, just pass through c_iter_
   RecyclingIterator *recyciter  // null if not Active Recycling; then, points to the iterator
  );

// the following lines are the interface that is shared with CompactionIterator, so these entry points
// must not be modified
  const Slice& key() { return  use_indirects_ ? key_ : c_iter_->key(); }
  const Slice& value() { return use_indirects_ ? value_ : c_iter_->value(); }
  const Status& status() { return use_indirects_ ? status_ : c_iter_->status(); }
  const ParsedInternalKey& ikey() { return use_indirects_ ? ikey_ : c_iter_->ikey(); }
    // If an end key (exclusive) is specified, check if the current key is
    // >= than it and return invalid if it is because the iterator is out of its range
  bool Valid() { return use_indirects_ ? valid_ : (c_iter_->Valid() && 
           !(end_ != nullptr && pcfd->user_comparator()->Compare(c_iter_->user_key(), *end_) >= 0)); }
  void Next();
// end of shared interface
  // Return the vector of earliest-references to each ring within the current file, and clear the value for next file.
  // This should be called AFTER the last key of the current file has been retrieved.
  // We initialize the references to high_value for ease in comparison; but when we return to the user we replace
  // high_value by 0 to indicate 'no reference' to that ring
  // Because of the way this is called from the compaction loop, Next() is called to look ahead one key before
  // closing the output file.  So, ref0_ does not include the last fileref that was returned.  On the very last file, we need to
  // include that key
  void ref0(std::vector<uint64_t>& result, bool include_last) {
    if(!use_indirects_){result = std::vector<uint64_t>(); return; }  // return null value if no indirects
    if(include_last)  // include the last key only for the last file
      if(ref0_[prevringfno.ringno]>prevringfno.fileno)
        ref0_[prevringfno.ringno]=prevringfno.fileno;  // if current > new, switch to new
    result = ref0_; for(size_t i=0;i<ref0_.size();++i){if(result[i]==high_value)result[i]=0; ref0_[i]=high_value;}
#if DEBLEVEL&4
printf("Iterator file info (include_last=%d): ",include_last);
    for(int i=0;i<result.size();++i)printf("%lld ",result[i]);
printf("\n");
#endif
    return;
  }

  // return the restart edit block, to be installed into the edit list for the compaction
  void getedit(std::vector<VLogRingRestartInfo>& result) {
// scaf must handle the case where there was no diskref & thus no diskdatalen
    result.clear();  // init result to empty (i. e. no changes)
    if(!use_indirects_) return;  // return null value if no indirects
    result.resize(ref0_.size());  // reserve space for all the rings
    // account for size added to the output ring
    if(fileendoffsets.size()){   // if there are no files, output no record, since a 0 record is a delete
      result[nextdiskref.Ringno()].size = diskdatalen;  // # bytes written
      result[nextdiskref.Ringno()].valid_files.push_back(firstdiskref.Fileno());  // output start,end of the added files
      result[nextdiskref.Ringno()].valid_files.push_back(firstdiskref.Fileno()+fileendoffsets.size()-1);
    }
    // account for fragmentation, added to any ring we read from
    for(int i=0;i<result.size();++i)result[i].frag = addedfrag[i];   // copy our internal calculation
  }

  // Indicate whether the current key/value is the last key/value in its file.  0=we don't know, 1=yes, -1=no
  // When we are outputting the last kv for the current file, we return 1 to request that the current output file be closed
  // We advance the output pointer until we hit the file containing the current record; then we output 'end' on the last record
  // This DOES NOT handle the case of SSTs with no records (they would need to skip over an empty output file), but those should not occur anyway
  int OverrideClose() { if(!use_indirects_ || filereccnts.size()==0) return 0; while(keyno_>filereccnts[outputfileno])outputfileno++; return (keyno_==filereccnts[outputfileno])?1:-1; }

private:
  Slice key_;  // the next key to return, if it is Valid()
  Slice value_;  // the next value to return, if it is Valid()
  Status status_;  // the status to return
  ParsedInternalKey ikey_;  // like key_, but parsed
  std::string npikey;  // string form of ikey_
  bool valid_;  // set when there is another kv to be read
  ColumnFamilyData* pcfd;  // ColumnFamilyData for this run
  CompactionIterator* c_iter_;  // underlying c_iter_, the source for our values
  Slice *end_;   // if given, the key+1 of the end of range
  bool use_indirects_;  // if false, just pass c_iter_ result through
  std::string keys;  // all the keys read from the iterator, jammed together
  std::vector<size_t> keylens;   // length of each string in keys
  size_t keysx_;   // position in keys[] where the next key starts
  std::string passthroughdata;  // data that is passed through unchanged
  std::vector<VLogRingRefFileOffset> passthroughrecl;  // record lengths (NOT running total) of records in passthroughdata
  std::vector<char> valueclass;   // one entry per key.  bit 0 means 'value is a passthrough'; bit 1 means 'value is being converted from direct to indirect'
  std::vector<VLogRingRefFileOffset> diskrecl;  // running total of record lengths in diskdata
  VLogRingRef firstdiskref;  // reference for the first data written to VLog
  VLogRingRef nextdiskref;  // reference for the next data to be written to VLog
  std::vector<VLogRingRefFileOffset>fileendoffsets;   // end+1 offset of the data written to successive VLog files  (starting offset is 0)
  std::vector<Status> inputerrorstatus;  // error status returned by the iterator
  std::vector<Status> outputerrorstatus;  // error status returned when writing the output files
  std::shared_ptr<VLog> current_vlog;
  std::vector<uint64_t> ref0_;  // for each ring, the earliest reference found into the ring.  Reset when we start each new file
  std::vector<int64_t> addedfrag;  // fragmentation added, for each ring
struct RingFno {
  int ringno;
  VLogRingRefFileno fileno;
};
  std::vector<RingFno> diskfileref;   // where we hold the reference values from the input passthroughs
  RingFno prevringfno;  // set to the ring/file for the key we are returning now.  It is not included in the ref0_ value until
    // the NEXT key is returned (this to match the way the compaction job uses the iterator), at which time it is the previous key to use
  std::vector<size_t> filereccnts;  // record# of the last kvs in each of the input files we encounter
  size_t outputfileno;  // For AR, the file number of the current kv being returned.  When it changes we call for a new file in the compaction

  int keyno_;  // number of keys processed previously
  int passx_;  // number of passthrough references returned previously
  int diskx_;  // number of disk references returned previously
  int filex_;  // number of files (as returned by RingWrite) that have been completely returned to the user
  int statusx_;  // number of input error statuses returned to user
  int ostatusx_;  // number of output error statuses returned to user
  int passthroughrefx_;  // number of passthrough indirects returned to user
  VLogRingRefFileOffset nextpassthroughref;  // index of next passthrough byte to return
  size_t diskdatalen;  // number of bytes written to disk

enum valtype : int { vNone=0, vIndirectRemapped=1, vPassthroughDirect=2, vIndirectFirstMap=3, vPassthroughIndirect=4, vHasError=8 };

};

} // namespace rocksdb

// Roadmap of what happens during compaction
//
// (under mutex)
// compaction-picker decides what files to compact
// (release mutex)
// ?? optionally splits the compaction into subcompactions
// subcompactions run.  Each creates a bunch of SSTs, VLog files
//   each SST contains its earliest reference to the VLog
//   SSTs are collected, by level, into subcompact->output_files
//   at the end of subcompaction edit block getedit() installs the Vlog restart info into the edit block.  It tells which VLog files were added, & how much size/frag was added by the subcompaction
// subcompactions finish; results collected into a list of edits, each containing a list of files   scaf ?? where
// (acquire mutex)
// in Install (compaction_job.cc) call AddFile/DeleteFile to add the output_files and input_files from the compaction into the edit block
// call LogAndApply to finish up:
// create a single-threading point:
//   put the request (edit list) onto a queue, called the writer queue
//   if some other task is working on the queue, block
//       this means that ALL code past this point, even if it releases the mutex, is guaranteed to be single-threaded
//
// process the queue.
// create a new Version block, v, into which the changes will be installed
//
// loop over the edits.  Process all the consecutive requests for the SAME cf (except for column-family changes).  This guarantees that
//   all the subcompactions for a given compaction are handled in the same run through this thread.  For each edit:
//    call Apply in the builder, which is a place to amalgamate the edits into a single block called rep_.  Deletes are matched with Adds here, so that during recovery
//       if a file is Added and then Deleted, it will be taken out of the edit list with no attempt to add the now-nonexistent SST.  Also amalgamate
//       the VLog restart info.  The result in rep_ is added_files and deleted_files for the SSTs, and vlog_additions for the VLog info
//
// after all edits have been processed, call SaveTo in the builder to move the SSTs into v.  This involves merging them with previous files in the version.
//   result is modified v.  new and retiring SSTs are processed against the VLogRing, to Install references for the new files and UnCurrent the files that are being deleted.
//   The VLog info is not connected to a Version, so this doesn't affect v.  After v has been created, the builder is destroyed.  Before it is, we save the
//   amalgamated VLog changes in a temp (accum_vlog_edits).  We also save the accumulated edits that were applied, in batch_edits.
//
// Now that the in-memory SSTs are right, what remains is to log to the Manifest.  This can take a long time, so the mutex is released to allow
// other threads to add their requests to the writer queue.  This path is still single-threaded, though.
// (release mutex)
//
// The Manifest accumulates edit blocks, the same kind that were processed during Apply (thus, recovery merely needs to read these edit blocks and
// feed them into Apply).  If the log of edits gets too big, it is replaced by a Snapshot of the current version (i. e. the Version BEFORE the current edits).
// Optionally write that Snapshot now.  Note that no VLog changes have been applied to the CF, so the VLog stats written to the Snapshot are old)
//
// NOW we apply the accum_vlog_edits to the CF, bringing the CF up to the new version.  After we have made those changes, we check to see if there
// are any VLog files that have just become unreferenced in the new version.  We remove any such from the stats in the CF, and also modify the batch_edits list to include
// the change so that the edits written next will indicate that these files are no longer in the database.  The file itself is not deleted, because
// it may be active in an old Version; it will be deleted when the last internal reference goes away or the database is recovered.
//
// Append all the batch_edits to the Manifest.  This brings the manifest up to the level of the new Version v, including VLog stats.
//
// Housekeep the manifest files, marking the one that is current, for recovery purposes.
//
// Install the new Version as the current Version.
//
// Wake up any tasks waiting on the writer queue, thus ending the single-threaded section.  (should wake just one, no?)


