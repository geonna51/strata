#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include "memtable.h"
#include "skiplist.h"
#include "test_util.h"

namespace {

struct IntComparator {
  int operator()(int a, int b) const {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
  }
};

void TestSkipListBasic() {
  std::cout << "Running TestSkipListBasic..." << std::endl;
  strata::SkipList<int, IntComparator> list(IntComparator{});

  ASSERT_FALSE(list.Contains(10));

  list.Insert(10);
  list.Insert(5);
  list.Insert(20);
  list.Insert(15);

  ASSERT_TRUE(list.Contains(5));
  ASSERT_TRUE(list.Contains(10));
  ASSERT_TRUE(list.Contains(15));
  ASSERT_TRUE(list.Contains(20));
  ASSERT_FALSE(list.Contains(7));

  // Forward iteration
  strata::SkipList<int, IntComparator>::Iterator iter(&list);
  iter.SeekToFirst();
  std::vector<int> sorted;
  while (iter.Valid()) {
    sorted.push_back(iter.key());
    iter.Next();
  }
  ASSERT_EQ(sorted.size(), 4);
  ASSERT_EQ(sorted[0], 5);
  ASSERT_EQ(sorted[1], 10);
  ASSERT_EQ(sorted[2], 15);
  ASSERT_EQ(sorted[3], 20);

  // Backward iteration
  iter.SeekToLast();
  std::vector<int> rev;
  while (iter.Valid()) {
    rev.push_back(iter.key());
    iter.Prev();
  }
  ASSERT_EQ(rev.size(), 4);
  ASSERT_EQ(rev[0], 20);
  ASSERT_EQ(rev[1], 15);
  ASSERT_EQ(rev[2], 10);
  ASSERT_EQ(rev[3], 5);

  // Seek
  iter.Seek(12);
  ASSERT_TRUE(iter.Valid());
  ASSERT_EQ(iter.key(), 15);

  iter.Seek(25);
  ASSERT_FALSE(iter.Valid());
}

void TestMemtableOperations() {
  std::cout << "Running TestMemtableOperations..." << std::endl;
  strata::Memtable mem;

  // Key not found initially
  std::string val;
  strata::Status s;
  ASSERT_FALSE(mem.Get("key1", 100, &val, &s));

  // Add key1 at seq 10
  mem.Add(10, strata::kTypeValue, "key1", "val1_seq10");
  ASSERT_TRUE(mem.Get("key1", 10, &val, &s));
  ASSERT_OK(s);
  ASSERT_EQ(val, "val1_seq10");

  // Querying at seq 5 should not find it
  ASSERT_FALSE(mem.Get("key1", 5, &val, &s));

  // Update key1 at seq 20
  mem.Add(20, strata::kTypeValue, "key1", "val1_seq20");
  ASSERT_TRUE(mem.Get("key1", 25, &val, &s));
  ASSERT_OK(s);
  ASSERT_EQ(val, "val1_seq20");

  // Reading at snapshot seq 15 should still see val1_seq10
  ASSERT_TRUE(mem.Get("key1", 15, &val, &s));
  ASSERT_OK(s);
  ASSERT_EQ(val, "val1_seq10");

  // Delete key1 at seq 30
  mem.Add(30, strata::kTypeDeletion, "key1", "");
  ASSERT_TRUE(mem.Get("key1", 35, &val, &s));
  ASSERT_NOT_FOUND(s);

  // But snapshot at seq 25 still sees it
  ASSERT_TRUE(mem.Get("key1", 25, &val, &s));
  ASSERT_OK(s);
  ASSERT_EQ(val, "val1_seq20");

  // Memory usage increases
  ASSERT_TRUE(mem.ApproximateMemoryUsage() > 0);
  ASSERT_EQ(mem.Count(), 3);
}

void TestMemtableIterator() {
  std::cout << "Running TestMemtableIterator..." << std::endl;
  strata::Memtable mem;

  mem.Add(1, strata::kTypeValue, "apple", "red");
  mem.Add(2, strata::kTypeValue, "banana", "yellow");
  mem.Add(3, strata::kTypeValue, "cherry", "dark-red");

  auto* it = mem.NewIterator();
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  ASSERT_EQ(it->Key(), "apple");
  ASSERT_EQ(it->Value(), "red");

  it->Next();
  ASSERT_TRUE(it->Valid());
  ASSERT_EQ(it->Key(), "banana");

  it->Next();
  ASSERT_TRUE(it->Valid());
  ASSERT_EQ(it->Key(), "cherry");

  it->Next();
  ASSERT_FALSE(it->Valid());

  // Seek
  it->Seek("b");
  ASSERT_TRUE(it->Valid());
  ASSERT_EQ(it->Key(), "banana");

  delete it;
}

}  // namespace

int main() {
  TestSkipListBasic();
  TestMemtableOperations();
  TestMemtableIterator();
  std::cout << "All Memtable tests passed!" << std::endl;
  return 0;
}
