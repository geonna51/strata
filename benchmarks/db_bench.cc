#include <chrono>
#include <iomanip>
#include <iostream>
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

  void Report() const {
    double ops_per_sec = (elapsed_seconds > 0) ? (static_cast<double>(num_ops) / elapsed_seconds) : 0.0;
    double mb_per_sec = (elapsed_seconds > 0) ? (static_cast<double>(bytes) / (1024.0 * 1024.0) / elapsed_seconds) : 0.0;
    double us_per_op = (num_ops > 0) ? (elapsed_seconds * 1e6 / static_cast<double>(num_ops)) : 0.0;

    std::cout << std::left << std::setw(16) << name
              << ": " << std::right << std::setw(10) << static_cast<size_t>(ops_per_sec) << " ops/sec; "
              << std::fixed << std::setprecision(1) << std::setw(6) << mb_per_sec << " MB/s; "
              << std::setprecision(2) << std::setw(8) << us_per_op << " us/op ("
              << std::setprecision(3) << elapsed_seconds << " s total)"
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
    } else if (bench_name == "readmissing") {
      BenchmarkReadMissing();
    } else {
      std::cerr << "Unknown benchmark: " << bench_name << std::endl;
    }
  }

 private:
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
    system(("rm -rf " + test_dir).c_str());

    dbpath_ = test_dir;
    OpenDB(sync, true);

    std::string val(val_size_, 'v');
    size_t total_bytes = 0;

    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < num_; ++i) {
      std::string key = GenerateKey(i);
      Status s = db_->Put(key, val);
      if (!s.ok()) {
        std::cerr << "Put error: " << s.ToString() << std::endl;
        break;
      }
      total_bytes += key.size() + val.size();
    }
    auto end = std::chrono::high_resolution_clock::now();

    BenchmarkStats stats;
    stats.name = sync ? "fillsync" : "fillseq";
    stats.num_ops = num_;
    stats.bytes = total_bytes;
    stats.elapsed_seconds = std::chrono::duration<double>(end - start).count();
    stats.Report();

    delete db_;
    db_ = nullptr;
  }

  void BenchmarkFillRandom() {
    std::string test_dir = dbpath_ + "_rand";
    system(("rm -rf " + test_dir).c_str());

    dbpath_ = test_dir;
    OpenDB(false, true);

    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<size_t> dist(0, num_ * 2);

    std::string val(val_size_, 'r');
    size_t total_bytes = 0;

    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < num_; ++i) {
      std::string key = GenerateKey(dist(rng));
      Status s = db_->Put(key, val);
      if (!s.ok()) break;
      total_bytes += key.size() + val.size();
    }
    auto end = std::chrono::high_resolution_clock::now();

    BenchmarkStats stats;
    stats.name = "fillrandom";
    stats.num_ops = num_;
    stats.bytes = total_bytes;
    stats.elapsed_seconds = std::chrono::duration<double>(end - start).count();
    stats.Report();
  }

  void BenchmarkReadSeq() {
    size_t count = 0;
    size_t total_bytes = 0;

    auto start = std::chrono::high_resolution_clock::now();
    std::unique_ptr<Iterator> it(db_->NewIterator());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      count++;
      total_bytes += it->Key().size() + it->Value().size();
    }
    auto end = std::chrono::high_resolution_clock::now();

    BenchmarkStats stats;
    stats.name = "readseq";
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

    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < num_; ++i) {
      std::string key = GenerateKey(dist(rng));
      Status s = db_->Get(key, &val);
      if (s.ok()) {
        found++;
        total_bytes += key.size() + val.size();
      }
    }
    auto end = std::chrono::high_resolution_clock::now();
    (void)found;

    BenchmarkStats stats;
    stats.name = "readrandom";
    stats.num_ops = num_;
    stats.bytes = total_bytes;
    stats.elapsed_seconds = std::chrono::duration<double>(end - start).count();
    stats.Report();
  }

  void BenchmarkReadMissing() {
    std::string val;
    size_t missing = 0;

    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < num_; ++i) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "missing:%010zu", i);
      Status s = db_->Get(buf, &val);
      if (s.IsNotFound()) {
        missing++;
      }
    }
    auto end = std::chrono::high_resolution_clock::now();
    (void)missing;

    BenchmarkStats stats;
    stats.name = "readmissing";
    stats.num_ops = num_;
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
  std::string bench_list = "fillseq,fillrandom,readseq,readrandom,readmissing";

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
  std::cout << "-----------------------------------------------------------------" << std::endl;

  strata::bench::Benchmark benchmark(dbpath, num, val_size);

  size_t pos = 0;
  while (pos < bench_list.size()) {
    size_t comma = bench_list.find(',', pos);
    std::string bench = (comma == std::string::npos) ? bench_list.substr(pos) : bench_list.substr(pos, comma - pos);
    benchmark.RunBenchmark(bench);
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }

  std::cout << "-----------------------------------------------------------------" << std::endl;
  return 0;
}
