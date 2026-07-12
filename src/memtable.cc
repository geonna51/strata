#include "memtable.h"

namespace strata {

Memtable::Memtable()
    : table_(MemtableKeyComparator()), memory_usage_(0), count_(0) {}

Memtable::~Memtable() = default;

void Memtable::Add(SequenceNumber seq, ValueType type, const Slice& key,
                   const Slice& value) {
  MemtableEntry entry;
  entry.user_key = key.ToString();
  entry.seq = seq;
  entry.type = type;
  entry.value = value.ToString();

  table_.Insert(entry);

  // Track heap usage: entry payload + string capacities + approximate SkipList node pointers
  size_t usage = sizeof(MemtableEntry) + entry.user_key.size() +
                 entry.value.size() + sizeof(void*) * 8;
  memory_usage_.fetch_add(usage, std::memory_order_relaxed);
  count_.fetch_add(1, std::memory_order_relaxed);
}

bool Memtable::Get(const Slice& key, SequenceNumber seq, std::string* value,
                   Status* s) {
  MemtableEntry probe;
  probe.user_key = key.ToString();
  probe.seq = seq;

  SkipList<MemtableEntry, MemtableKeyComparator>::Iterator iter(&table_);
  iter.Seek(probe);
  if (iter.Valid()) {
    const MemtableEntry& entry = iter.key();
    if (entry.user_key == probe.user_key) {
      if (entry.type == kTypeValue) {
        if (value != nullptr) {
          *value = entry.value;
        }
        *s = Status::OK();
        return true;
      } else if (entry.type == kTypeDeletion) {
        *s = Status::NotFound();
        return true;
      }
    }
  }
  return false;
}

size_t Memtable::ApproximateMemoryUsage() const {
  return memory_usage_.load(std::memory_order_relaxed);
}

Memtable::MemTableIterator* Memtable::NewIterator() {
  return new MemTableIterator(&table_);
}

Memtable::MemTableIterator::MemTableIterator(
    const SkipList<MemtableEntry, MemtableKeyComparator>* list)
    : iter_(list) {}

bool Memtable::MemTableIterator::Valid() const {
  return iter_.Valid();
}

void Memtable::MemTableIterator::SeekToFirst() {
  iter_.SeekToFirst();
}

void Memtable::MemTableIterator::SeekToLast() {
  iter_.SeekToLast();
}

void Memtable::MemTableIterator::Seek(const std::string& target) {
  MemtableEntry probe;
  probe.user_key = target;
  probe.seq = kMaxSequenceNumber;
  iter_.Seek(probe);
}

void Memtable::MemTableIterator::Next() {
  iter_.Next();
}

void Memtable::MemTableIterator::Prev() {
  iter_.Prev();
}

std::string Memtable::MemTableIterator::Key() const {
  return iter_.key().user_key;
}

std::string Memtable::MemTableIterator::Value() const {
  return iter_.key().value;
}

Status Memtable::MemTableIterator::status() const {
  return Status::OK();
}

SequenceNumber Memtable::MemTableIterator::Seq() const {
  assert(Valid());
  return iter_.key().seq;
}

ValueType Memtable::MemTableIterator::Type() const {
  assert(Valid());
  return iter_.key().type;
}

const MemtableEntry& Memtable::MemTableIterator::Entry() const {
  return iter_.key();
}

}  // namespace strata
