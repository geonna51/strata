#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "strata/db.h"
#include "strata/options.h"
#include "test_util.h"

namespace {

void RunConcurrencyTorture() {
  std::cout << "Starting Concurrency Torture Test: 8 Readers, 1 Continuous Writer, Constant Compaction..." << std::endl;
  std::string dbpath = "/tmp/strata_concurrency_torture_db";
  system(("rm -rf " + dbpath).c_str());

  strata::Options options;
  options.create_if_missing = true;
  // Tiny 4KB memtable forces near-continuous flushes to Level 0
  options.write_buffer_size = 4096;
  options.max_level0_files = 3;  // Triggers major compaction quickly
  options.block_cache_capacity_bytes = 16 * 1024 * 1024;

  strata::DB* db = nullptr;
  ASSERT_OK(strata::DB::Open(options, dbpath, &db));

  std::atomic<bool> stop_flag{false};
  std::atomic<uint64_t> total_writes{0};
  std::atomic<uint64_t> total_reads{0};
  std::atomic<uint64_t> total_scans{0};
  std::atomic<uint64_t> errors{0};

  constexpr size_t kKeyUniverse = 1000;

  // 1 Continuous Writer Thread
  std::thread writer([&]() {
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<size_t> dist(0, kKeyUniverse - 1);

    while (!stop_flag.load(std::memory_order_relaxed)) {
      size_t k = dist(rng);
      char key[32];
      snprintf(key, sizeof(key), "key:%06zu", k);
      std::string val = "value:" + std::to_string(k);

      strata::Status s = db->Put(key, val);
      if (!s.ok()) {
        errors.fetch_add(1);
        break;
      }
      total_writes.fetch_add(1, std::memory_order_relaxed);
    }
  });

  // 4 Point-Lookup Reader Threads
  std::vector<std::thread> point_readers;
  for (int t = 0; t < 4; ++t) {
    point_readers.emplace_back([&, t]() {
      std::mt19937_64 rng(100 + t);
      std::uniform_int_distribution<size_t> dist(0, kKeyUniverse - 1);

      while (!stop_flag.load(std::memory_order_relaxed)) {
        size_t k = dist(rng);
        char key[32];
        snprintf(key, sizeof(key), "key:%06zu", k);
        std::string val;

        strata::Status s = db->Get(key, &val);
        if (s.ok()) {
          std::string expected = "value:" + std::to_string(k);
          if (val != expected) {
            std::cerr << "Inconsistent value read for key " << key << ": got " << val
                      << " expected " << expected << std::endl;
            errors.fetch_add(1);
          }
        } else if (!s.IsNotFound()) {
          std::cerr << "Unexpected error during Get: " << s.ToString() << std::endl;
          errors.fetch_add(1);
        }
        total_reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // 4 Range-Scan Iterator Reader Threads
  std::vector<std::thread> scan_readers;
  for (int t = 0; t < 4; ++t) {
    scan_readers.emplace_back([&, t]() {
      std::mt19937_64 rng(200 + t);
      std::uniform_int_distribution<size_t> dist(0, kKeyUniverse - 1);

      while (!stop_flag.load(std::memory_order_relaxed)) {
        size_t k = dist(rng);
        char target[32];
        snprintf(target, sizeof(target), "key:%06zu", k);

        strata::Iterator* it = db->NewIterator();
        it->Seek(target);

        std::string last_key = "";
        int steps = 0;
        while (it->Valid() && steps < 20) {
          std::string curr_key = it->Key();
          if (!last_key.empty() && curr_key <= last_key) {
            std::cerr << "Iterator ordering violation: " << curr_key << " <= " << last_key << std::endl;
            errors.fetch_add(1);
          }
          last_key = curr_key;
          it->Next();
          steps++;
        }

        if (!it->status().ok()) {
          std::cerr << "Iterator error: " << it->status().ToString() << std::endl;
          errors.fetch_add(1);
        }

        delete it;
        total_scans.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // Let the torture run for 3 seconds
  std::this_thread::sleep_for(std::chrono::milliseconds(3000));
  stop_flag.store(true, std::memory_order_relaxed);

  writer.join();
  for (auto& r : point_readers) r.join();
  for (auto& s : scan_readers) s.join();

  std::cout << "Concurrency Torture Finished:" << std::endl;
  std::cout << "  Writes executed: " << total_writes.load() << std::endl;
  std::cout << "  Point reads:     " << total_reads.load() << std::endl;
  std::cout << "  Range scans:     " << total_scans.load() << std::endl;
  std::cout << "  Errors detected: " << errors.load() << std::endl;

  ASSERT_EQ(errors.load(), 0);
  ASSERT_TRUE(total_writes.load() > 1000);
  ASSERT_TRUE(total_reads.load() > 5000);
  ASSERT_TRUE(total_scans.load() > 1000);

  delete db;
  system(("rm -rf " + dbpath).c_str());
  std::cout << "PASS: Concurrency torture test passed with 0 lifetime or consistency errors." << std::endl;
}

}  // namespace

int main() {
  RunConcurrencyTorture();
  return 0;
}
