#pragma once

#include <cstddef>

namespace strata {

struct Options {
  bool create_if_missing = true;
  bool error_if_exists = false;

  // Maximum size of in-memory write buffer (Memtable) before flushing to SSTable.
  size_t write_buffer_size = 4 * 1024 * 1024;  // 4MB

  // If true, WAL writes are flushed and fsync'd before returning success.
  // Defaults to false for high throughput asynchronous I/O.
  bool sync = false;

  // Target size for SSTable data blocks.
  size_t block_size = 4096;  // 4KB

  // Approximate number of bits per key allocated in Bloom filter.
  // 10 bits per key yields ~1% false positive rate.
  int bloom_bits_per_key = 10;

  // Capacity in bytes for the uncompressed block cache.
  size_t block_cache_capacity_bytes = 8 * 1024 * 1024;  // 8MB

  // Trigger compaction when Level 0 contains at least this many files.
  int max_level0_files = 4;

  // Target file size for Level 1 and higher SSTables.
  size_t max_file_size = 2 * 1024 * 1024;  // 2MB
};

}  // namespace strata
