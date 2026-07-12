#pragma once

#include <memory>
#include <vector>
#include "iterator_internal.h"

namespace strata {

class MergingIterator : public InternalIterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<InternalIterator>> children);
  ~MergingIterator() override = default;

  MergingIterator(const MergingIterator&) = delete;
  MergingIterator& operator=(const MergingIterator&) = delete;

  bool Valid() const override;
  void SeekToFirst() override;
  void SeekToLast() override;
  void Seek(const std::string& target) override;
  void Next() override;
  void Prev() override;
  std::string Key() const override;
  std::string Value() const override;
  Status status() const override;

  SequenceNumber Seq() const override;
  ValueType Type() const override;

 private:
  void FindSmallest();
  void FindLargest();

  std::vector<std::unique_ptr<InternalIterator>> children_;
  InternalIterator* current_;
  Status status_;
};

InternalIterator* NewMergingIterator(
    std::vector<std::unique_ptr<InternalIterator>> children);

}  // namespace strata
