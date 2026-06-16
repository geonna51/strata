#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <vector>
#include "block.h"
#include "block_cache.h"
#include "bloom_filter.h"
#include "sstable_builder.h"
#include "sstable_reader.h"
#include "test_util.h"

namespace {

void TestBlockDirect() {
  std::cout << "Running TestBlockDirect..." << std::endl;
  strata::BlockBuilder builder(2);

  builder.Add(1, strata::kTypeValue, "apple", "red");
  builder.Add(2, strata::kTypeValue, "banana", "yellow");
  builder.Add(3, strata::kTypeValue, "cherry", "dark-red");
  builder.Add(4, strata::kTypeValue, "date", "brown");

  strata::Slice slice = builder.Finish();
  strata::Block block(std::string(slice.data(), slice.size()));

  std::unique_ptr<strata::Block::Iter> iter(block.NewIterator());
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->Key(), "apple");
  ASSERT_EQ(iter->Value(), "red");

  iter->Next();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->Key(), "banana");

  iter->Next();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->Key(), "cherry");

  iter->Next();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->Key(), "date");

  iter->Next();
  ASSERT_FALSE(iter->Valid());

  // Test Seek
  iter->Seek("cat");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->Key(), "cherry");

  iter->Seek("date");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->Key(), "date");

  iter->Seek("zebra");
  ASSERT_FALSE(iter->Valid());
}

void TestBloomFilter() {
  std::cout << "Running TestBloomFilter..." << std::endl;
  std::vector<std::string> keys;
  for (int i = 0; i < 2000; ++i) {
    keys.push_back("user:key:" + std::to_string(i));
  }

  std::string filter;
  strata::BloomFilter::CreateFilter(keys, 10, &filter);
  ASSERT_TRUE(filter.size() > 0);

  // 100% of inserted keys must match
  for (const auto& key : keys) {
    ASSERT_TRUE(strata::BloomFilter::KeyMayMatch(key, filter));
  }

  // False positive rate check
  int false_positives = 0;
  int test_count = 10000;
  for (int i = 2000; i < 2000 + test_count; ++i) {
    std::string absent_key = "nonexistent:key:" + std::to_string(i);
    if (strata::BloomFilter::KeyMayMatch(absent_key, filter)) {
      false_positives++;
    }
  }

  double fp_rate = static_cast<double>(false_positives) / test_count;
  std::cout << "Bloom filter false positive rate: " << (fp_rate * 100.0) << "%" << std::endl;
  // With 10 bits/key, false positive rate should be ~1%, certainly < 2.5%
  ASSERT_TRUE(fp_rate < 0.025);
}

void TestBlockCacheLRU() {
  std::cout << "Running TestBlockCacheLRU..." << std::endl;
  // Capacity for 2 blocks of 100 bytes each
  strata::BlockCache cache(200);

  auto b1 = std::make_shared<strata::Block>(std::string(100, 'a'));
  auto b2 = std::make_shared<strata::Block>(std::string(100, 'b'));
  auto b3 = std::make_shared<strata::Block>(std::string(100, 'c'));

  cache.Insert(1, 0, b1, 100);
  cache.Insert(1, 100, b2, 100);

  ASSERT_TRUE(cache.Lookup(1, 0) != nullptr);
  ASSERT_TRUE(cache.Lookup(1, 100) != nullptr);

  // Inserting b3 should evict b1 (since b1 was accessed before b2, b1 is LRU)
  cache.Insert(1, 200, b3, 100);
  ASSERT_TRUE(cache.Lookup(1, 0) == nullptr);
  ASSERT_TRUE(cache.Lookup(1, 100) != nullptr);
  ASSERT_TRUE(cache.Lookup(1, 200) != nullptr);
}

void TestSSTableBuildAndRead() {
  std::cout << "Running TestSSTableBuildAndRead..." << std::endl;
  std::string sst_path = "/tmp/strata_test_table.sst";
  ::unlink(sst_path.c_str());

  strata::Options options;
  options.block_size = 512;  // Small block size to create multiple blocks

  // Build table with 500 keys
  {
    strata::SSTableBuilder builder(options, sst_path);
    for (int i = 0; i < 500; ++i) {
      char key[32];
      snprintf(key, sizeof(key), "key_%06d", i);
      std::string val = "value_" + std::to_string(i);
      builder.Add(i + 1, strata::kTypeValue, key, val);
    }
    ASSERT_OK(builder.Finish());
    ASSERT_EQ(builder.NumEntries(), 500);
    ASSERT_TRUE(builder.FileSize() > 0);
  }

  // Read table
  {
    strata::BlockCache cache(1024 * 1024);
    std::shared_ptr<strata::SSTableReader> reader;
    // Get file size
    struct stat s;
    ASSERT_EQ(::stat(sst_path.c_str(), &s), 0);
    ASSERT_OK(strata::SSTableReader::Open(options, sst_path, 1, s.st_size, &reader));

    // Test Point Lookups
    for (int i = 0; i < 500; ++i) {
      char key[32];
      snprintf(key, sizeof(key), "key_%06d", i);
      std::string expected_val = "value_" + std::to_string(i);

      std::string val;
      strata::Status st;
      bool found = reader->Get(key, 10000, &val, &st, &cache);
      ASSERT_TRUE(found);
      ASSERT_OK(st);
      ASSERT_EQ(val, expected_val);
    }

    // Test nonexistent keys
    std::string val;
    strata::Status st;
    ASSERT_FALSE(reader->Get("key_999999", 10000, &val, &st, &cache));
    ASSERT_FALSE(reader->Get("nonexistent_key", 10000, &val, &st, &cache));

    // Test Table Iterator
    std::unique_ptr<strata::Iterator> iter(reader->NewIterator(&cache));
    iter->SeekToFirst();
    int count = 0;
    while (iter->Valid()) {
      char key[32];
      snprintf(key, sizeof(key), "key_%06d", count);
      ASSERT_EQ(iter->Key(), key);
      count++;
      iter->Next();
    }
    ASSERT_EQ(count, 500);

    // Test Seek
    iter->Seek("key_000250");
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->Key(), "key_000250");
  }

  ::unlink(sst_path.c_str());
}

}  // namespace

int main() {
  TestBlockDirect();
  TestBloomFilter();
  TestBlockCacheLRU();
  TestSSTableBuildAndRead();
  std::cout << "All SSTable tests passed!" << std::endl;
  return 0;
}
