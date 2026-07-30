#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include "block.h"
#include "block_cache.h"
#include "format.h"
#include "iterator_internal.h"
#include "strata/iterator.h"
#include "strata/options.h"
#include "strata/slice.h"
#include "strata/status.h"

namespace strata {

class SSTableReader : public std::enable_shared_from_this<SSTableReader> {
 public:
  static Status Open(const Options& options, const std::string& filename,
                     uint64_t file_number, uint64_t file_size,
                     std::shared_ptr<SSTableReader>* reader);

  ~SSTableReader();

  SSTableReader(const SSTableReader&) = delete;
  SSTableReader& operator=(const SSTableReader&) = delete;

  // Search for user_key with sequence <= seq.
  // Returns true if found (value populated and *s is OK, or tombstone and *s is NotFound).
  // Returns false if not present in this SSTable.
  bool Get(const Slice& user_key, SequenceNumber seq, std::string* value,
           Status* s, BlockCache* cache = nullptr);

  // Return a two-level iterator over all entries in the SSTable.
  InternalIterator* NewIterator(BlockCache* cache = nullptr);

  uint64_t FileNumber() const { return file_number_; }
  uint64_t FileSize() const { return file_size_; }

  friend class TwoLevelIterator;

 private:
  SSTableReader(const Options& options, const std::string& filename,
                uint64_t file_number, uint64_t file_size, int fd);

  Status ReadBlock(const BlockHandle& handle, std::shared_ptr<Block>* result);

  Options options_;
  std::string filename_;
  uint64_t file_number_;
  uint64_t file_size_;
  int fd_;

  std::shared_ptr<Block> index_block_;
  std::string filter_data_;
};

}  // namespace strata
