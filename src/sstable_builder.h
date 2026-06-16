#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "block.h"
#include "format.h"
#include "strata/options.h"
#include "strata/slice.h"
#include "strata/status.h"

namespace strata {

class SSTableBuilder {
 public:
  SSTableBuilder(const Options& options, const std::string& filename);
  ~SSTableBuilder();

  SSTableBuilder(const SSTableBuilder&) = delete;
  SSTableBuilder& operator=(const SSTableBuilder&) = delete;

  // Add an entry to the SSTable being constructed.
  // Entries must be added in strictly sorted order by internal key comparator.
  void Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value);

  // Finishes building the table and closes the output file.
  // Returns Status::OK() on success.
  Status Finish();

  // Returns number of key/value entries added so far.
  uint64_t NumEntries() const { return num_entries_; }

  // Returns current size of the file on disk.
  uint64_t FileSize() const { return offset_; }

  // Returns smallest and largest user keys added.
  const std::string& SmallestKey() const { return smallest_key_; }
  const std::string& LargestKey() const { return largest_key_; }

 private:
  void FlushBlock();
  Status WriteRawBlock(const Slice& block_contents, BlockHandle* handle);

  Options options_;
  std::string filename_;
  int fd_;
  uint64_t offset_;
  uint64_t num_entries_;

  BlockBuilder data_block_;
  BlockBuilder index_block_;

  std::vector<std::string> keys_for_filter_;
  std::string last_key_in_block_;
  std::string smallest_key_;
  std::string largest_key_;

  Status status_;
  bool closed_;
};

}  // namespace strata
