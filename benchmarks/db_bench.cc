#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include "strata/db.h"
#include "strata/options.h"

namespace strata {
namespace bench {

struct BenchmarkStats {
  std::string name;
  size_t num_ops = 0;
  size_t bytes = 0;
  double elapsed_seconds = 0.0;
  std::vector<double> latencies_us;

  void Report() {
    double ops_per_sec = (elapsed_seconds > 0) ? (static_cast<double>(num_ops) / elapsed_seconds) : 0.0;
    double mb_per_sec = (elapsed_seconds > 0) ? (static_cast<double>(bytes) / (1024.0 * 1024.0) / elapsed_seconds) : 0.0;
    double avg_us = (num_ops > 0) ? (elapsed_seconds * 1e6 / static_cast<double>(num_ops)) : 0.0;

    double p50 = avg_us, p95 = avg_us, p99 = avg_us;
    if (!latencies_us.empty()) {
      std::sort(latencies_us.begin(), latencies_us.end());
      size_t n = latencies_us.size();
      p50 = latencies_us[static_cast<size_t>(n * 0.50)];
      p95 = latencies_us[static_cast<size_t>(n * 0.95)];
      p99 = latencies_us[static_cast<size_t>(n * 0.99)];
    }

    std::cout << std::left << std::setw(14) << name
              << ": " << std::right << std::setw(9) << static_cast<size_t>(ops_per_sec) << " ops/sec; "
              << std::fixed << std::setprecision(1) << std::setw(6) << mb_per_sec << " MB/s; "
              << "p50: " << std::setprecision(2) << std::setw(6) << p50 << " us; "
              << "p95: " << std::setprecision(2) << std::setw(6) << p95 << " us; "
              << "p99: " << std::setprecision(2) << std::setw(6) << p99 << " us ("
              << std::setprecision(3) << elapsed_seconds << " s)"
              << std::endl;
  }
};

class Benchmark {
 public:
  Benchmark(const std::string& dbpath, size_t num, size_t val_size)
      : dbpath_(dbpath), num_(num), val_size_(val_size), db_(nullptr) {}

  ~Benchmark() {
    delete db_;
  }

  void RunBenchmark(const std::string& bench_name) {
    if (bench_name == "fillseq") {
      BenchmarkFillSeq(false);
    } else if (bench_name == "fillsync") {
      BenchmarkFillSeq(true);
    } else if (bench_name == "fillrandom") {
      BenchmarkFillRandom();
    } else if (bench_name == "readseq") {
      BenchmarkReadSeq();
    } else if (bench_name == "readrandom") {
      BenchmarkReadRandom();
    } else if (bench_name == "readcold") {
      BenchmarkReadCold();
    } else if (bench_name == "readmissing") {
      BenchmarkReadMissing();
    } else {
      std::cerr << "Unknown benchmark: " << bench_name << std::endl;
    }
  }

 private:
  static void CleanDir(const std::string& path) {
    int rc = ::system(("rm -rf " + path).c_str());
    (void)rc;
  }

  std::string GenerateKey(size_t i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key:%010zu", i);
    return std::string(buf);
  }

  void OpenDB(bool sync, bool create) {
    delete db_;
    db_ = nullptr;

    Options options;
    options.create_if_missing = create;
    options.sync = sync;
    options.write_buffer_size = 4 * 1024 * 1024;
    options.block_size = 4096;
    options.bloom_bits_per_key = 10;
    options.block_cache_capacity_bytes = 64 * 1024 * 1024;

    Status s = DB::Open(options, dbpath_, &db_);
    if (!s.ok()) {
      std::cerr << "DB::Open failed: " << s.ToString() << std::endl;
      std::exit(1);
    }
  }

  void BenchmarkFillSeq(bool sync) {
    std::string test_dir = dbpath_ + (sync ? "_sync" : "_seq");
    CleanDir(test_dir);

    dbpath_ = test_dir;
    OpenDB(sync, true);

    std::string val(val_size_, 'v');
    size_t total_bytes = 0;

    BenchmarkStats stats;
    stats.name = sync ? "fillsync" : "fillseq";
    stats.num_ops = num_;
    stats.latencies_us.reserve(std::min(num_, size_t(100000)));

    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < num_; ++i) {
      std::string key = GenerateKey(i);
      auto op_start = std::chrono::high_resolution_clock::now();
      Status s = db_->Put(key, val);
      auto op_end = std::chrono::high_resolution_clock::now();
      if (stats.latencies_us.size() < 100000) {
        stats.latencies_us.push_back(
            std::chrono::duration<double, std::micro>(op_end - op_start).count());
      }
      if (!s.ok()) {
        std::cerr << "Put error: " << s.ToString() << std::endl;
        break;
      }
      total_bytes += key.size() + val.size();
    }
    auto end = std::chrono::high_resolution_clock::now();

    stats.bytes = total_bytes;
    stats.elapsed_seconds = std::chrono::duration<double>(end - start).count();
    stats.Report();

    delete db_;
    db_ = nullptr;
  }

  void BenchmarkFillRandom() {
    std::string test_dir = dbpath_ + "_rand";
    CleanDir(test_dir);

    dbpath_ = test_dir;
    OpenDB(false, true);

    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<size_t> dist(0, num_ * 2);

    std::string val(val_size_, 'r');
    size_t total_bytes = 0;

    BenchmarkStats stats;
    stats.name = "fillrandom";
    stats.num_ops = num_;
    stats.latencies_us.reserve(std::min(num_, size_t(100000)));

    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < num_; ++i) {
      std::string key = GenerateKey(dist(rng));
      auto op_start = std::chrono::high_resolution_clock::now();
      Status s = db_->Put(key, val);
      auto op_end = std::chrono::high_resolution_clock::now();
      if (stats.latencies_us.size() < 100000) {
        stats.latencies_us.push_back(
            std::chrono::duration<double, std::micro>(op_end - op_start).count());
      }
      if (!s.ok()) break;
      total_bytes += key.size() + val.size();
    }
    auto end = std::chrono::high_resolution_clock::now();

    stats.bytes = total_bytes;
    stats.elapsed_seconds = std::chrono::duration<double>(end - start).count();
    stats.Report();
  }

  void BenchmarkReadSeq() {
    size_t count = 0;
    size_t total_bytes = 0;

    BenchmarkStats stats;
    stats.name = "readseq";

    auto start = std::chrono::high_resolution_clock::now();
    std::unique_ptr<Iterator> it(db_->NewIterator());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      count++;
      total_bytes += it->Key().size() + it->Value().size();
    }
    auto end = std::chrono::high_resolution_clock::now();

    stats.num_ops = count;
    stats.bytes = total_bytes;
    stats.elapsed_seconds = std::chrono::duration<double>(end - start).count();
    stats.Report();
  }

  void BenchmarkReadRandom() {
    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<size_t> dist(0, num_ * 2);

    std::string val;
    size_t found = 0;
    size_t total_bytes = 0;

    BenchmarkStats stats;
    stats.name = "readrandom";
    stats.num_ops = num_;
    stats.latencies_us.reserve(std::min(num_, size_t(100000)));

    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < num_; ++i) {
      std::string key = GenerateKey(dist(rng));
      auto op_start = std::chrono::high_resolution_clock::now();
      Status s = db_->Get(key, &val);
      auto op_end = std::chrono::high_resolution_clock::now();
      if (stats.latencies_us.size() < 100000) {
        stats.latencies_us.push_back(
            std::chrono::duration<double, std::micro>(op_end - op_start).count());
      }
      if (s.ok()) {
        found++;
        total_bytes += key.size() + val.size();
      }
    }
    auto end = std::chrono::high_resolution_clock::now();
    (void)found;

    stats.bytes = total_bytes;
    stats.elapsed_seconds = std::chrono::duration<double>(end - start).count();
    stats.Report();
  }

  void BenchmarkReadCold() {
    // Reopen DB to flush and purge in-memory caches
    OpenDB(false, false);

    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<size_t> dist(0, num_ * 2);

    std::string val;
    size_t found = 0;
    size_t total_bytes = 0;

    BenchmarkStats stats;
    stats.name = "readcold";
    stats.num_ops = num_;
    stats.latencies_us.reserve(std::min(num_, size_t(100000)));

    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < num_; ++i) {
      std::string key = GenerateKey(dist(rng));
      auto op_start = std::chrono::high_resolution_clock::now();
      Status s = db_->Get(key, &val);
      auto op_end = std::chrono::high_resolution_clock::now();
      if (stats.latencies_us.size() < 100000) {
        stats.latencies_us.push_back(
            std::chrono::duration<double, std::micro>(op_end - op_start).count());
      }
      if (s.ok()) {
        found++;
        total_bytes += key.size() + val.size();
      }
    }
    auto end = std::chrono::high_resolution_clock::now();
    (void)found;

    stats.bytes = total_bytes;
    stats.elapsed_seconds = std::chrono::duration<double>(end - start).count();
    stats.Report();
  }

  void BenchmarkReadMissing() {
    std::string val;
    size_t missing = 0;

    BenchmarkStats stats;
    stats.name = "readmissing";
    stats.num_ops = num_;
    stats.latencies_us.reserve(std::min(num_, size_t(100000)));

    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < num_; ++i) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "missing:%010zu", i);
      auto op_start = std::chrono::high_resolution_clock::now();
      Status s = db_->Get(buf, &val);
      auto op_end = std::chrono::high_resolution_clock::now();
      if (stats.latencies_us.size() < 100000) {
        stats.latencies_us.push_back(
            std::chrono::duration<double, std::micro>(op_end - op_start).count());
      }
      if (s.IsNotFound()) {
        missing++;
      }
    }
    auto end = std::chrono::high_resolution_clock::now();
    (void)missing;

    stats.bytes = num_ * 16;  // key lookup bytes
    stats.elapsed_seconds = std::chrono::duration<double>(end - start).count();
    stats.Report();
  }

  std::string dbpath_;
  size_t num_;
  size_t val_size_;
  DB* db_;
};

}  // namespace bench
}  // namespace strata

int main(int argc, char* argv[]) {
  size_t num = 100000;
  size_t val_size = 100;
  std::string dbpath = "/tmp/strata_bench_db";
  std::string bench_list = "fillseq,fillrandom,readseq,readrandom,readcold,readmissing";

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--num=", 0) == 0) {
      num = std::stoull(arg.substr(6));
    } else if (arg.rfind("--value_size=", 0) == 0) {
      val_size = std::stoull(arg.substr(13));
    } else if (arg.rfind("--benchmarks=", 0) == 0) {
      bench_list = arg.substr(13);
    } else if (arg.rfind("--db=", 0) == 0) {
      dbpath = arg.substr(5);
    }
  }

  std::cout << "Strata Benchmark Suite" << std::endl;
  std::cout << "Entries:        " << num << std::endl;
  std::cout << "Value size:     " << val_size << " bytes" << std::endl;
  std::cout << "Raw data size:  " << (num * (val_size + 16) / (1024 * 1024)) << " MB" << std::endl;
  std::cout << "--------------------------------------------------------------------------------------------------------" << std::endl;

  strata::bench::Benchmark benchmark(dbpath, num, val_size);

  size_t pos = 0;
  while (pos < bench_list.size()) {
    size_t comma = bench_list.find(',', pos);
    std::string bench = (comma == std::string::npos) ? bench_list.substr(pos) : bench_list.substr(pos, comma - pos);
    benchmark.RunBenchmark(bench);
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }

  std::cout << "--------------------------------------------------------------------------------------------------------" << std::endl;
  return 0;
}
