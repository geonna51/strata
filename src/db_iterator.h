#pragma once

#include <memory>
#include <string>
#include "format.h"
#include "iterator_internal.h"
#include "strata/iterator.h"
#include "strata/status.h"

namespace strata {

class DBIterator : public Iterator {
 public:
  DBIterator(std::unique_ptr<InternalIterator> iter, SequenceNumber sequence);
  ~DBIterator() override = default;

  DBIterator(const DBIterator&) = delete;
  DBIterator& operator=(const DBIterator&) = delete;

  bool Valid() const override { return valid_; }
  void SeekToFirst() override;
  void SeekToLast() override;
  void Seek(const std::string& target) override;
  void Next() override;
  void Prev() override;
  std::string Key() const override;
  std::string Value() const override;
  Status status() const override;

 private:
  void FindNextUserEntry(bool seeking);

  std::unique_ptr<InternalIterator> iter_;
  SequenceNumber sequence_;
  bool valid_;
  std::string saved_key_;
  std::string saved_value_;
};

}  // namespace strata
