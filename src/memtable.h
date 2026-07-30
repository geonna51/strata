#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include "format.h"
#include "skiplist.h"
#include "strata/iterator.h"
#include "strata/slice.h"
#include "strata/status.h"

#include "iterator_internal.h"

namespace strata {

struct MemtableEntry {
  std::string user_key;
  SequenceNumber seq = 0;
  ValueType type = kTypeValue;
  std::string value;
};

struct MemtableKeyComparator {
  int operator()(const MemtableEntry& a, const MemtableEntry& b) const {
    return CompareInternalKey(a.user_key, a.seq, b.user_key, b.seq);
  }
};

class Memtable {
 public:
  Memtable();
  ~Memtable();

  Memtable(const Memtable&) = delete;
  Memtable& operator=(const Memtable&) = delete;

  // Insert a new record into the memtable.
  void Add(SequenceNumber seq, ValueType type, const Slice& key,
           const Slice& value);

  // Look up a key with sequence number <= seq.
  // Returns true if the key was found (either active value or tombstone).
  // If active value, *s is Status::OK() and *value is populated.
  // If tombstone, *s is Status::NotFound().
  // Returns false if key was not found in this memtable.
  bool Get(const Slice& key, SequenceNumber seq, std::string* value, Status* s);

  // Approximate memory consumed by this memtable in bytes.
  size_t ApproximateMemoryUsage() const;

  // Returns number of entries in the memtable.
  size_t Count() const { return count_.load(std::memory_order_relaxed); }

  class MemTableIterator : public InternalIterator {
   public:
    explicit MemTableIterator(const SkipList<MemtableEntry, MemtableKeyComparator>* list,
                              std::shared_ptr<Memtable> pin = nullptr);
    ~MemTableIterator() override = default;

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

    const MemtableEntry& Entry() const;

   private:
    std::shared_ptr<Memtable> pin_;
    typename SkipList<MemtableEntry, MemtableKeyComparator>::Iterator iter_;
  };

  MemTableIterator* NewIterator(std::shared_ptr<Memtable> pin = nullptr);

 private:
  SkipList<MemtableEntry, MemtableKeyComparator> table_;
  std::atomic<size_t> memory_usage_;
  std::atomic<size_t> count_;
};

}  // namespace strata
