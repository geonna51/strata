#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "strata/db.h"
#include "test_util.h"

namespace {

void TestDBBasicPutGetDelete() {
  std::cout << "Running TestDBBasicPutGetDelete..." << std::endl;
  std::string dbpath = "/tmp/strata_test_db_basic";
  system(("rm -rf " + dbpath).c_str());

  strata::Options options;
  options.create_if_missing = true;

  strata::DB* db = nullptr;
  ASSERT_OK(strata::DB::Open(options, dbpath, &db));

  // Key doesn't exist yet
  std::string val;
  ASSERT_NOT_FOUND(db->Get("user:123", &val));

  // Put key
  ASSERT_OK(db->Put("user:123", "George"));
  ASSERT_OK(db->Get("user:123", &val));
  ASSERT_EQ(val, "George");

  // Delete key (Tombstone)
  ASSERT_OK(db->Delete("user:123"));
  ASSERT_NOT_FOUND(db->Get("user:123", &val));

  delete db;
  system(("rm -rf " + dbpath).c_str());
}

void TestDBOverwritesAndOrdering() {
  std::cout << "Running TestDBOverwritesAndOrdering..." << std::endl;
  std::string dbpath = "/tmp/strata_test_db_overwrites";
  system(("rm -rf " + dbpath).c_str());

  strata::Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 1024;  // Small buffer to trigger flushes

  strata::DB* db = nullptr;
  ASSERT_OK(strata::DB::Open(options, dbpath, &db));

  // Put key1 = A
  ASSERT_OK(db->Put("key1", "A"));

  // Fill up enough keys to force flush of key1 to SSTable
  for (int i = 0; i < 50; ++i) {
    ASSERT_OK(db->Put("pad:" + std::to_string(i), std::string(100, 'x')));
  }

  // Put key1 = B (now sits in active Memtable while A is in SSTable)
  ASSERT_OK(db->Put("key1", "B"));

  std::string val;
  ASSERT_OK(db->Get("key1", &val));
  // Must return "B" (newest version)
  ASSERT_EQ(val, "B");

  delete db;
  system(("rm -rf " + dbpath).c_str());
}

void TestDBIterators() {
  std::cout << "Running TestDBIterators..." << std::endl;
  std::string dbpath = "/tmp/strata_test_db_iter";
  system(("rm -rf " + dbpath).c_str());

  strata::Options options;
  options.create_if_missing = true;

  strata::DB* db = nullptr;
  ASSERT_OK(strata::DB::Open(options, dbpath, &db));

  ASSERT_OK(db->Put("user:100", "Alice"));
  ASSERT_OK(db->Put("user:101", "Bob"));
  ASSERT_OK(db->Put("user:102", "Charlie"));
  ASSERT_OK(db->Put("other:001", "SomethingElse"));

  // Range scan for prefix "user:"
  strata::Iterator* it = db->NewIterator();
  ASSERT_TRUE(it != nullptr);

  std::vector<std::pair<std::string, std::string>> results;
  for (it->Seek("user:100"); it->Valid() && it->Key().rfind("user:", 0) == 0; it->Next()) {
    results.emplace_back(it->Key(), it->Value());
  }
  ASSERT_OK(it->status());
  delete it;

  ASSERT_EQ(results.size(), 3);
  ASSERT_EQ(results[0].first, "user:100");
  ASSERT_EQ(results[0].second, "Alice");
  ASSERT_EQ(results[1].first, "user:101");
  ASSERT_EQ(results[1].second, "Bob");
  ASSERT_EQ(results[2].first, "user:102");
  ASSERT_EQ(results[2].second, "Charlie");

  // Overwrite user:101 and delete user:100
  ASSERT_OK(db->Put("user:101", "BobUpdated"));
  ASSERT_OK(db->Delete("user:100"));

  it = db->NewIterator();
  results.clear();
  for (it->Seek("user:100"); it->Valid() && it->Key().rfind("user:", 0) == 0; it->Next()) {
    results.emplace_back(it->Key(), it->Value());
  }
  ASSERT_OK(it->status());
  delete it;

  ASSERT_EQ(results.size(), 2);
  ASSERT_EQ(results[0].first, "user:101");
  ASSERT_EQ(results[0].second, "BobUpdated");
  ASSERT_EQ(results[1].first, "user:102");
  ASSERT_EQ(results[1].second, "Charlie");

  delete db;
  system(("rm -rf " + dbpath).c_str());
}

void TestDBPersistenceAndRestart() {
  std::cout << "Running TestDBPersistenceAndRestart..." << std::endl;
  std::string dbpath = "/tmp/strata_test_db_restart";
  system(("rm -rf " + dbpath).c_str());

  strata::Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 4096;

  // 1. Write records and close
  {
    strata::DB* db = nullptr;
    ASSERT_OK(strata::DB::Open(options, dbpath, &db));
    for (int i = 0; i < 500; ++i) {
      ASSERT_OK(db->Put("key:" + std::to_string(i), "value:" + std::to_string(i)));
    }
    delete db;
  }

  // 2. Re-open and verify all 500 records are preserved
  {
    strata::DB* db = nullptr;
    ASSERT_OK(strata::DB::Open(options, dbpath, &db));
    for (int i = 0; i < 500; ++i) {
      std::string val;
      ASSERT_OK(db->Get("key:" + std::to_string(i), &val));
      ASSERT_EQ(val, "value:" + std::to_string(i));
    }
    delete db;
  }

  system(("rm -rf " + dbpath).c_str());
}

void TestDBConcurrentReadWrite() {
  std::cout << "Running TestDBConcurrentReadWrite..." << std::endl;
  std::string dbpath = "/tmp/strata_test_db_concurrent";
  system(("rm -rf " + dbpath).c_str());

  strata::Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 16384;

  strata::DB* db = nullptr;
  ASSERT_OK(strata::DB::Open(options, dbpath, &db));

  std::atomic<bool> stop_flag{false};

  // Writer thread
  std::thread writer([&]() {
    for (int i = 0; i < 1000; ++i) {
      db->Put("c_key:" + std::to_string(i), "c_val:" + std::to_string(i));
    }
    stop_flag = true;
  });

  // Reader thread
  std::thread reader([&]() {
    while (!stop_flag) {
      std::string val;
      db->Get("c_key:10", &val);
    }
  });

  writer.join();
  reader.join();

  // Verify all written keys exist
  for (int i = 0; i < 1000; ++i) {
    std::string val;
    ASSERT_OK(db->Get("c_key:" + std::to_string(i), &val));
    ASSERT_EQ(val, "c_val:" + std::to_string(i));
  }

  delete db;
  system(("rm -rf " + dbpath).c_str());
}

}  // namespace

int main() {
  TestDBBasicPutGetDelete();
  TestDBOverwritesAndOrdering();
  TestDBIterators();
  TestDBPersistenceAndRestart();
  TestDBConcurrentReadWrite();
  std::cout << "All DB integration tests passed successfully!" << std::endl;
  return 0;
}
