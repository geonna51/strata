#include "db_iterator.h"
#include <cassert>

namespace strata {

DBIterator::DBIterator(std::unique_ptr<InternalIterator> iter, SequenceNumber sequence,
                       std::shared_ptr<Version> version_pin)
    : iter_(std::move(iter)),
      sequence_(sequence),
      version_pin_(std::move(version_pin)),
      valid_(false) {}

void DBIterator::FindNextUserEntry(bool /*seeking*/) {
  valid_ = false;
  while (iter_->Valid()) {
    if (iter_->Seq() > sequence_) {
      iter_->Next();
      continue;
    }

    std::string candidate_key = iter_->Key();
    if (iter_->Type() == kTypeValue) {
      saved_key_ = candidate_key;
      saved_value_ = iter_->Value();
      valid_ = true;
      return;
    } else if (iter_->Type() == kTypeDeletion) {
      // Tombstone: skip this key and all older versions of this key
      while (iter_->Valid() && iter_->Key() == candidate_key) {
        iter_->Next();
      }
    } else {
      iter_->Next();
    }
  }
}

void DBIterator::SeekToFirst() {
  if (!iter_) return;
  iter_->SeekToFirst();
  FindNextUserEntry(true);
}

void DBIterator::SeekToLast() {
  if (!iter_) return;
  // Position by seeking to first and scanning to end
  SeekToFirst();
  std::string last_k, last_v;
  bool had_any = false;
  while (Valid()) {
    last_k = Key();
    last_v = Value();
    had_any = true;
    Next();
  }
  if (had_any) {
    saved_key_ = last_k;
    saved_value_ = last_v;
    valid_ = true;
  }
}

void DBIterator::Seek(const std::string& target) {
  if (!iter_) return;
  iter_->Seek(target);
  FindNextUserEntry(true);
}

void DBIterator::Next() {
  assert(Valid());
  std::string current_key = saved_key_;
  while (iter_->Valid() && iter_->Key() == current_key) {
    iter_->Next();
  }
  FindNextUserEntry(false);
}

void DBIterator::Prev() {
  assert(Valid());
  std::string target = saved_key_;
  SeekToFirst();
  std::string prev_k, prev_v;
  bool found_prev = false;
  while (Valid() && Key() < target) {
    prev_k = Key();
    prev_v = Value();
    found_prev = true;
    Next();
  }
  if (found_prev) {
    saved_key_ = prev_k;
    saved_value_ = prev_v;
    valid_ = true;
  } else {
    valid_ = false;
  }
}

std::string DBIterator::Key() const {
  assert(Valid());
  return saved_key_;
}

std::string DBIterator::Value() const {
  assert(Valid());
  return saved_value_;
}

Status DBIterator::status() const {
  if (!iter_) return Status::OK();
  return iter_->status();
}

}  // namespace strata
