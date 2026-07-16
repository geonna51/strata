#include "merging_iterator.h"
#include <cassert>

namespace strata {

static int CompareIterators(const InternalIterator* a, const InternalIterator* b) {
  int r = a->Key().compare(b->Key());
  if (r != 0) return r;
  if (a->Seq() > b->Seq()) return -1;
  if (a->Seq() < b->Seq()) return +1;
  if (a->Type() > b->Type()) return -1;
  if (a->Type() < b->Type()) return +1;
  return 0;
}

MergingIterator::MergingIterator(std::vector<std::unique_ptr<InternalIterator>> children)
    : children_(std::move(children)), current_(nullptr), status_(Status::OK()) {}

bool MergingIterator::Valid() const {
  return current_ != nullptr && current_->Valid();
}

void MergingIterator::FindSmallest() {
  InternalIterator* smallest = nullptr;
  for (const auto& child : children_) {
    if (child->Valid()) {
      if (smallest == nullptr || CompareIterators(child.get(), smallest) < 0) {
        smallest = child.get();
      }
    }
  }
  current_ = smallest;
}

void MergingIterator::FindLargest() {
  InternalIterator* largest = nullptr;
  for (const auto& child : children_) {
    if (child->Valid()) {
      if (largest == nullptr || CompareIterators(child.get(), largest) > 0) {
        largest = child.get();
      }
    }
  }
  current_ = largest;
}

void MergingIterator::SeekToFirst() {
  for (auto& child : children_) {
    child->SeekToFirst();
  }
  FindSmallest();
}

void MergingIterator::SeekToLast() {
  for (auto& child : children_) {
    child->SeekToLast();
  }
  FindLargest();
}

void MergingIterator::Seek(const std::string& target) {
  for (auto& child : children_) {
    child->Seek(target);
  }
  FindSmallest();
}

void MergingIterator::Next() {
  assert(Valid());
  current_->Next();
  FindSmallest();
}

void MergingIterator::Prev() {
  assert(Valid());
  current_->Prev();
  FindLargest();
}

std::string MergingIterator::Key() const {
  assert(Valid());
  return current_->Key();
}

std::string MergingIterator::Value() const {
  assert(Valid());
  return current_->Value();
}

SequenceNumber MergingIterator::Seq() const {
  assert(Valid());
  return current_->Seq();
}

ValueType MergingIterator::Type() const {
  assert(Valid());
  return current_->Type();
}

Status MergingIterator::status() const {
  for (const auto& child : children_) {
    if (!child->status().ok()) {
      return child->status();
    }
  }
  return Status::OK();
}

InternalIterator* NewMergingIterator(
    std::vector<std::unique_ptr<InternalIterator>> children) {
  if (children.empty()) {
    return nullptr;
  }
  return new MergingIterator(std::move(children));
}

}  // namespace strata
