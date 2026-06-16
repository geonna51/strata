#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "coding.h"
#include "format.h"
#include "strata/iterator.h"
#include "strata/slice.h"
#include "strata/status.h"

namespace strata {

struct BlockHandle {
  uint64_t offset = 0;
  uint64_t size = 0;

  void EncodeTo(std::string* dst) const {
    PutFixed64(dst, offset);
    PutFixed64(dst, size);
  }

  Status DecodeFrom(Slice* input) {
    if (input->size() < 16) {
      return Status::Corruption("Bad block handle");
    }
    offset = DecodeFixed64(input->data());
    size = DecodeFixed64(input->data() + 8);
    input->remove_prefix(16);
    return Status::OK();
  }
};

class BlockBuilder {
 public:
  explicit BlockBuilder(int restart_interval = 16);

  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  void Reset();
  void Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value);
  Slice Finish();
  size_t CurrentSizeEstimate() const;
  bool Empty() const { return buffer_.empty(); }

 private:
  int restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int counter_;
  bool finished_;
};

class Block {
 public:
  explicit Block(std::string data);
  ~Block() = default;

  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  size_t size() const { return data_.size(); }

  class Iter : public Iterator {
   public:
    explicit Iter(const Block* block);
    ~Iter() override = default;

    bool Valid() const override { return current_ < restarts_offset_; }
    void SeekToFirst() override;
    void SeekToLast() override;
    void Seek(const std::string& target) override;
    void Next() override;
    void Prev() override;
    std::string Key() const override { return current_key_; }
    std::string Value() const override { return current_value_; }
    Status status() const override { return status_; }

    SequenceNumber Seq() const { return current_seq_; }
    ValueType Type() const { return current_type_; }

   private:
    void ParseEntryAt(uint32_t offset);
    uint32_t GetRestartOffset(uint32_t index) const;

    const Block* block_;
    const char* data_;
    uint32_t restarts_offset_;
    uint32_t num_restarts_;

    uint32_t current_;
    uint32_t next_offset_;
    std::string current_key_;
    SequenceNumber current_seq_;
    ValueType current_type_;
    std::string current_value_;
    Status status_;
  };

  Iter* NewIterator() const { return new Iter(this); }

 private:
  friend class Iter;
  std::string data_;
  uint32_t restarts_offset_;
  uint32_t num_restarts_;
};

}  // namespace strata
