#pragma once

#include <cstdint>
#include <string>
#include "format.h"
#include "strata/slice.h"
#include "strata/status.h"

namespace strata {

class WalWriter {
 public:
  explicit WalWriter(const std::string& filename);
  ~WalWriter();

  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  Status AppendRecord(SequenceNumber seq, ValueType type,
                      const Slice& key, const Slice& value, bool sync);
  Status Sync();
  Status Close();
  uint64_t FileSize() const { return file_size_; }

 private:
  std::string filename_;
  int fd_;
  uint64_t file_size_;
};

class WalReader {
 public:
  explicit WalReader(const std::string& filename);
  ~WalReader();

  WalReader(const WalReader&) = delete;
  WalReader& operator=(const WalReader&) = delete;

  // Reads the next record from the log.
  // Returns:
  // - Status::OK() on successful read.
  // - Status::NotFound() when reaching end of file.
  // - Status::Corruption() if data is corrupt.
  // When allow_partial_eof is true, an incomplete trailing record at EOF
  // (such as from a crash or SIGKILL) is treated as a clean EOF.
  Status ReadRecord(SequenceNumber* seq, ValueType* type,
                    std::string* key, std::string* value,
                    bool allow_partial_eof = true);

 private:
  std::string filename_;
  int fd_;
};

}  // namespace strata
