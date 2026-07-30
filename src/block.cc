#include "block.h"

namespace strata {

BlockBuilder::BlockBuilder(int restart_interval)
    : restart_interval_(restart_interval), counter_(0), finished_(false) {
  restarts_.push_back(0);
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);
  counter_ = 0;
  finished_ = false;
}

void BlockBuilder::Add(SequenceNumber seq, ValueType type, const Slice& key,
                       const Slice& value) {
  assert(!finished_);
  if (counter_ >= restart_interval_) {
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }

  PutFixed32(&buffer_, static_cast<uint32_t>(key.size()));
  buffer_.append(key.data(), key.size());
  PutFixed64(&buffer_, seq);
  buffer_.push_back(static_cast<char>(type));
  PutFixed32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(value.data(), value.size());
  counter_++;
}

Slice BlockBuilder::Finish() {
  for (uint32_t r : restarts_) {
    PutFixed32(&buffer_, r);
  }
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return Slice(buffer_);
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  return buffer_.size() + restarts_.size() * sizeof(uint32_t) + sizeof(uint32_t);
}

Block::Block(std::string data)
    : data_(std::move(data)), restarts_offset_(0), num_restarts_(0) {
  if (data_.size() >= sizeof(uint32_t)) {
    num_restarts_ = DecodeFixed32(data_.data() + data_.size() - sizeof(uint32_t));
    size_t max_restarts = (data_.size() - sizeof(uint32_t)) / sizeof(uint32_t);
    if (num_restarts_ <= max_restarts) {
      restarts_offset_ = static_cast<uint32_t>(
          data_.size() - (1 + num_restarts_) * sizeof(uint32_t));
    } else {
      num_restarts_ = 0;
    }
  }
}

Block::Iter::Iter(const Block* block)
    : block_(block),
      data_(block_->data_.data()),
      restarts_offset_(block_->restarts_offset_),
      num_restarts_(block_->num_restarts_),
      current_(restarts_offset_),
      next_offset_(0),
      current_seq_(0),
      current_type_(kTypeValue),
      status_(Status::OK()) {}

uint32_t Block::Iter::GetRestartOffset(uint32_t index) const {
  assert(index < num_restarts_);
  return DecodeFixed32(data_ + restarts_offset_ + index * sizeof(uint32_t));
}

void Block::Iter::ParseEntryAt(uint32_t offset) {
  if (offset >= restarts_offset_) {
    current_ = restarts_offset_;
    return;
  }

  // Ensure minimum header size: klen(4) + seq(8) + type(1) + vlen(4) = 17 bytes
  if (static_cast<uint64_t>(offset) + 17 > restarts_offset_) {
    status_ = Status::Corruption("Truncated block entry");
    current_ = restarts_offset_;
    return;
  }

  uint32_t klen = DecodeFixed32(data_ + offset);
  if (static_cast<uint64_t>(offset) + 17 + klen > restarts_offset_) {
    status_ = Status::Corruption("Truncated block entry key");
    current_ = restarts_offset_;
    return;
  }

  current_key_ = std::string(data_ + offset + 4, klen);
  current_seq_ = DecodeFixed64(data_ + offset + 4 + klen);
  current_type_ = static_cast<ValueType>(data_[offset + 4 + klen + 8]);

  uint32_t vlen = DecodeFixed32(data_ + offset + 4 + klen + 9);
  if (static_cast<uint64_t>(offset) + 17 + klen + vlen > restarts_offset_) {
    status_ = Status::Corruption("Truncated block entry value");
    current_ = restarts_offset_;
    return;
  }

  current_value_ = std::string(data_ + offset + 4 + klen + 13, vlen);
  current_ = offset;
  next_offset_ = offset + 17 + klen + vlen;
}

void Block::Iter::SeekToFirst() {
  if (num_restarts_ == 0) {
    current_ = restarts_offset_;
    return;
  }
  ParseEntryAt(0);
}

void Block::Iter::SeekToLast() {
  if (num_restarts_ == 0) {
    current_ = restarts_offset_;
    return;
  }
  uint32_t offset = GetRestartOffset(num_restarts_ - 1);
  while (true) {
    ParseEntryAt(offset);
    if (next_offset_ >= restarts_offset_) {
      break;
    }
    offset = next_offset_;
  }
}

void Block::Iter::Seek(const std::string& target) {
  if (num_restarts_ == 0) {
    current_ = restarts_offset_;
    return;
  }

  int left = 0;
  int right = static_cast<int>(num_restarts_) - 1;
  int target_restart = 0;

  while (left <= right) {
    int mid = left + (right - left) / 2;
    uint32_t offset = GetRestartOffset(mid);
    uint32_t klen = DecodeFixed32(data_ + offset);
    std::string_view mid_key(data_ + offset + 4, klen);
    if (mid_key < target) {
      target_restart = mid;
      left = mid + 1;
    } else {
      right = mid - 1;
    }
  }

  uint32_t offset = GetRestartOffset(target_restart);
  while (offset < restarts_offset_) {
    ParseEntryAt(offset);
    if (!Valid()) {
      break;
    }
    if (current_key_ >= target) {
      return;
    }
    offset = next_offset_;
  }
  current_ = restarts_offset_;
}

void Block::Iter::Next() {
  assert(Valid());
  ParseEntryAt(next_offset_);
}

void Block::Iter::Prev() {
  assert(Valid());
  const uint32_t original = current_;
  int left = 0;
  int right = static_cast<int>(num_restarts_) - 1;
  int target_restart = 0;

  while (left <= right) {
    int mid = left + (right - left) / 2;
    if (GetRestartOffset(mid) < original) {
      target_restart = mid;
      left = mid + 1;
    } else {
      right = mid - 1;
    }
  }

  uint32_t offset = GetRestartOffset(target_restart);
  if (offset >= original) {
    current_ = restarts_offset_;
    return;
  }

  while (true) {
    ParseEntryAt(offset);
    if (next_offset_ >= original) {
      break;
    }
    offset = next_offset_;
  }
}

}  // namespace strata
