#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "format.h"
#include "sstable_reader.h"
#include "strata/iterator.h"
#include "strata/options.h"
#include "strata/slice.h"
#include "strata/status.h"
#include "wal.h"

namespace strata {

static constexpr int kNumLevels = 7;

struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  std::string smallest_key;
  std::string largest_key;
  mutable std::shared_ptr<SSTableReader> reader;
};

class VersionEdit {
 public:
  VersionEdit() = default;

  void Clear();
  void SetComparatorName(const std::string& name);
  void SetLogNumber(uint64_t num);
  void SetNextFile(uint64_t num);
  void SetLastSequence(uint64_t seq);
  void DeleteFile(int level, uint64_t file);
  void AddFile(int level, uint64_t file, uint64_t file_size,
               const std::string& smallest, const std::string& largest,
               std::shared_ptr<SSTableReader> reader = nullptr);

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(const Slice& src);

  bool has_comparator_ = false;
  std::string comparator_name_;

  bool has_log_number_ = false;
  uint64_t log_number_ = 0;

  bool has_next_file_number_ = false;
  uint64_t next_file_number_ = 0;

  bool has_last_sequence_ = false;
  uint64_t last_sequence_ = 0;

  std::set<std::pair<int, uint64_t>> deleted_files_;
  std::vector<std::pair<int, FileMetaData>> new_files_;
};

class Version {
 public:
  explicit Version(std::string dbname);
  ~Version();

  Version(const Version&) = delete;
  Version& operator=(const Version&) = delete;

  // Search through levels in order: L0 (newest to oldest), then L1..L6.
  // Returns true if key was found in some SSTable (either valid value or tombstone).
  bool Get(const Options& options, const Slice& user_key, SequenceNumber seq,
           std::string* value, Status* s, BlockCache* cache);

  // Appends iterators for all SSTables in this version.
  void AddIterators(const Options& options, BlockCache* cache,
                    std::vector<Iterator*>* iters);

  int NumLevelFiles(int level) const {
    return static_cast<int>(files_[level].size());
  }

  const std::vector<FileMetaData>& Files(int level) const {
    return files_[level];
  }

  uint64_t LevelBytes(int level) const;

 private:
  friend class VersionSet;
  std::string dbname_;
  std::vector<FileMetaData> files_[kNumLevels];
  mutable std::mutex reader_mutex_;
};

class VersionSet {
 public:
  VersionSet(std::string dbname, const Options* options, BlockCache* cache);
  ~VersionSet();

  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  // Apply edit to current version and persist changes to MANIFEST file.
  Status LogAndApply(VersionEdit* edit);

  // Recover state from persistent MANIFEST file.
  Status Recover();

  std::shared_ptr<Version> current() const { return current_; }
  uint64_t LastSequence() const { return last_sequence_; }
  void SetLastSequence(uint64_t s) {
    if (s > last_sequence_) last_sequence_ = s;
  }

  uint64_t LogNumber() const { return log_number_; }
  uint64_t NewFileNumber() { return next_file_number_++; }
  void ReuseFileNumber(uint64_t num) {
    if (next_file_number_ == num + 1) {
      next_file_number_ = num;
    }
  }

  // Populate live_files with file numbers that are currently referenced.
  void AddLiveFiles(std::set<uint64_t>* live_files) const;

  const Options* options() const { return options_; }
  BlockCache* cache() const { return cache_; }

 private:
  Status WriteSnapshot(WalWriter* writer);

  std::string dbname_;
  const Options* options_;
  BlockCache* cache_;
  std::shared_ptr<Version> current_;
  uint64_t next_file_number_;
  uint64_t manifest_file_number_;
  uint64_t last_sequence_;
  uint64_t log_number_;
  std::unique_ptr<WalWriter> descriptor_log_;
};

}  // namespace strata
