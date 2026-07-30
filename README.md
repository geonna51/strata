# Strata

[![Strata CI](https://github.com/geonna51/strata/actions/workflows/ci.yml/badge.svg)](https://github.com/geonna51/strata/actions/workflows/ci.yml)
[![C++17](https://img.shields.io/badge/standard-C%2B%2B17-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B17)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Dependencies](https://img.shields.io/badge/dependencies-zero-brightgreen.svg)]()

Strata is a high-performance, embedded Key-Value storage engine written from scratch in modern C++17 based on a **Log-Structured Merge (LSM) tree**. It is completely self-contained with **zero external dependencies**—built strictly on the C++17 standard library and POSIX systems interfaces.

---

## Architecture Overview

Traditional B-tree storage engines perform random in-place updates to disk pages. On both solid-state drives and rotating media, random writes incur high write amplification, cause I/O stalls, and degrade flash endurance.

Strata implements an LSM-tree architecture where all mutations are converted into sequential writes:

![Strata Architecture](docs/assets/architecture.svg)

### The Write Path
1. **Append to Write-Ahead Log (WAL)**: The mutation (`Put` or `Delete`) is serialized to an append-only log file with an 8-byte framing header and an IEEE 802.3 CRC32 checksum. This guarantees durability across process crashes and sudden power loss.
2. **Insert into Memtable**: In the same operation, the key-value pair is inserted into an in-memory concurrent SkipList sorted by user key and sequence number.
3. **Memtable Flush**: When the active memtable reaches its configured write buffer capacity (default 4 MB), it is marked immutable. A background worker thread flushes it sequentially to a new Level 0 SSTable file on disk and reclaims the memory.
4. **Multi-Level Compaction**: When Level 0 accumulates 4 or more SSTables (or deeper levels exceed their size thresholds), a background compaction thread merges overlapping sorted tables, discards superseded versions, and removes expired tombstones.

### The Read Path
Point lookups (`db->Get(key, &value)`) check storage tiers in order of recency:
1. **Active Memtable** in RAM (lock-free SkipList lookup)
2. **Immutable Memtable** in RAM (awaiting flush)
3. **Level 0 SSTables** on disk (newest to oldest; user keys may overlap across files)
4. **Level 1 through Level 5 SSTables** on disk (strictly partitioned non-overlapping key ranges; requires at most one SSTable probe per level)

At each candidate SSTable, Strata evaluates an in-memory MurmurHash3 Bloom filter (10 bits per key) to reject absent keys with a ~1% false positive rate without issuing disk I/O. If the filter matches, Strata checks its LRU block cache before reading the 4KB data block from storage.

---

## Engineering Design & Systems Correctness

Building a basic LSM tree is straightforward. Making it reliable under concurrent queries, continuous background compaction, and abrupt power failure requires careful systems design.

### 1. Concurrent Read & Compaction Lifetime (Version Pinning)
* **The Challenge**: A background compaction thread merges multiple Level 0 files into a new Level 1 file, then removes (`unlink`) the old SSTables from disk. If application threads are concurrently scanning iterators or executing `Get()` queries on those exact files, how do you prevent dangling pointers or read faults?
* **The Solution**: Strata uses version pinning with `std::shared_ptr<Version>`. When a reader begins a point lookup or allocates an `Iterator`, it acquires a shared pointer to the current database version under a brief mutex lock. `SSTableReader` instances inherit from `std::enable_shared_from_this`, ensuring file descriptors and cached metadata remain valid for the duration of the query.
* **POSIX Semantics**: On POSIX filesystems (macOS and Linux), calling `unlink()` removes the directory entry, but the operating system **preserves the inode and physical file blocks as long as an open file descriptor exists**. Readers continue reading from their open descriptors without interruption. When the last reference drops, the kernel reclaims the storage blocks.

### 2. Crash Consistency & Directory Durability
* **The Challenge**: Updating metadata files or renaming files (e.g. `rename("CURRENT.tmp", "CURRENT")`) updates directory entries in operating system memory, but does **not** flush those directory entries to physical non-volatile storage. A sudden power loss can cause directory state to roll back, leaving dangling references to missing files.
* **The Solution**: Strata enforces strict metadata ordering via [`src/env.h`](src/env.h):
  1. **Compaction File Ordering**: When compaction finishes generating new SSTables, it `fsync`s the new `.sst` files, then calls `SyncDirectory(dbname)` on the parent directory *before* recording the `VersionEdit` in the `MANIFEST`. This guarantees all new files physically exist in the directory before the manifest references them.
  2. **Atomic Manifest Pointer**: To update the active manifest, Strata writes the new manifest filename to `CURRENT.tmp`, `fsync`s the file, atomically renames it to `CURRENT`, and then `fsync`s the database directory (`fcntl(fd, F_FULLFSYNC)` on macOS, `fsync(fd)` on Linux).

### 3. Read Amplification Defense (Bloom Filters & Block Cache)
* **The Challenge**: In an LSM tree, a missing key could theoretically reside in any SSTable across multiple levels, resulting in dozens of disk lookups for a single negative query.
* **The Solution**:
  - **Bloom Filters**: Every SSTable contains a Bloom filter bitset (10 bits/key) using MurmurHash3. An in-memory bitset probe (~80 ns) bypasses 99% of unnecessary disk block reads.
  - **LRU Block Cache**: Uncompressed 4KB data blocks are cached in an in-memory concurrent LRU cache, accelerating repeat lookups and hot-spot range queries.

### 4. WAL Framing & Recovery from Torn Writes
* **The Challenge**: If power fails mid-write while appending to the Write-Ahead Log, a partially written record will remain at the end of the file.
* **The Solution**: Every WAL record consists of an 8-byte framing header followed by the record payload:
  ```
  Header (8 Bytes)
  ┌─────────────────────────┬─────────────────────────┐
  │   CRC32 Checksum (4B)   │   Payload Length (4B)   │
  └─────────────────────────┴─────────────────────────┘

  Payload
  ┌──────────┬──────────────┬──────────────┬──────────┬────────────────┬────────────┐
  │ Type(1B) │ Seq Num (8B) │ Key Len (4B) │ Key Data │ Value Len (4B) │ Value Data │
  └──────────┴──────────────┴──────────────┴──────────┴────────────────┴────────────┘
  ```
  The IEEE 802.3 CRC32 checksum validates both the payload length and the entire payload content. During crash recovery on `DB::Open()`:
  - If corruption occurs mid-log (bit flip or truncated length), Strata halts and returns `Status::Corruption` to prevent silent data loss.
  - If an incomplete record is detected at the physical tail of the file (EOF), Strata recognizes a torn write, truncates the incomplete tail, and recovers all acknowledged records up to the crash point.

---

## On-Disk SSTable Binary Layout

SSTables are immutable, ordered files structured for sequential I/O, binary search, and single-seek index resolution:

![Strata SSTable Format](docs/assets/sstable-format.svg)

- **4KB Data Blocks**: Keys are prefix-compressed (`shared_len` + `non_shared_len`) to minimize storage overhead. Every 16 keys, a restart point records the full key offset, enabling binary search within each 4KB block without linear decompression from the beginning.
- **Fixed 48-Byte Footer**: When opening an SSTable, Strata seeks directly to `file_size - 48` bytes using a single `pread()`. It validates the 8-byte magic number (`0xdb4772617461`, ASCII for `"Grata"`) and reads the index block handle, loading the index block into memory without scanning the rest of the file.

---

## Benchmarks & Performance

Measured on Apple Silicon (M2 Pro, 12 cores, macOS Darwin, Release build with `-O3` Clang) across 100,000 operations with 100-byte values:

![Strata Throughput Benchmark](docs/assets/benchmark-throughput.svg)

![Strata Latency Distribution](docs/assets/benchmark-latency.svg)

| Benchmark | Throughput | Bandwidth | p50 Latency | p95 Latency | p99 Latency | System Description |
| :--- | :---: | :---: | :---: | :---: | :---: | :--- |
| **`readmissing`** | **7,021,999 ops/s** | 107 MB/s | **0.08 µs** | 0.08 µs | 0.08 µs | Bloom filter detects absent keys in ~80 ns; zero disk I/O |
| **`readseq`** (Range scan) | **1,695,389 ops/s** | 184 MB/s | **avg 0.59 µs/entry** | — | — | Merging iterator streaming sequential entries across sorted blocks |
| **`readrandom`** (Warm cache) | **1,113,525 ops/s** | 121 MB/s | **0.83 µs** | 1.25 µs | 1.58 µs | Random point lookups hitting the 64MB in-memory LRU block cache |
| **`readreopen`** (Cold cache) | **866,685 ops/s** | 94 MB/s | **0.67 µs** | 1.08 µs | 13.62 µs | Strata LRU cache cleared on DB reopen; OS page cache warm |
| **`fillrandom`** (Writes) | **134,213 ops/s** | 15 MB/s | **3.21 µs** | 4.50 µs | 7.71 µs | Concurrent SkipList insert and sequential WAL append |
| **`fillseq`** (Sequential) | **122,003 ops/s** | 13 MB/s | **3.12 µs** | 5.46 µs | 9.96 µs | In-order SkipList insert and sequential WAL append |
| **`fillsync`** (Hardware sync) | **238 ops/s** | 0.03 MB/s | **4.05 ms** | 5.03 ms | 5.52 ms | Synchronous physical storage barrier (`F_FULLFSYNC`) per write |

### Systems Context & Analysis
- **Caching & Working Set**: 100,000 keys with 100-byte values produce ~11 MB of user data. In `readreopen`, the database is reopened with an empty Strata LRU block cache, requiring index and data blocks to be re-read. Because modern operating systems keep recently accessed pages in kernel page cache, `readreopen` measures block-cache-cold, page-cache-warm performance rather than raw flash media seek time.
- **Range Scans**: `readseq` streams through an active multi-way `Iterator` without per-operation timer sampling, reporting the aggregate average time per iterated record (0.59 µs/entry).
- **Synchronous Writes**: `fillsync` issues a hardware barrier (`fcntl(fd, F_FULLFSYNC)` on macOS, `fdatasync()` on Linux) on every individual write, reflecting the physical 4–5 ms write cache flush cycle of flash storage controllers.

---

## Quick Start Example

```cpp
#include <iostream>
#include <memory>
#include <string>
#include "strata/db.h"

int main() {
  strata::Options options;
  options.create_if_missing = true;
  options.block_cache_capacity_bytes = 64 * 1024 * 1024; // 64MB LRU cache

  strata::DB* db = nullptr;
  strata::Status s = strata::DB::Open(options, "/tmp/strata_demo", &db);
  if (!s.ok()) {
    std::cerr << "Failed to open database: " << s.ToString() << std::endl;
    return 1;
  }

  // 1. Put a key-value pair
  s = db->Put("user:1001", "George");

  // 2. Get the value
  std::string value;
  s = db->Get("user:1001", &value);
  if (s.ok()) {
    std::cout << "user:1001 => " << value << std::endl;
  }

  // 3. Scan a key range with an Iterator
  std::unique_ptr<strata::Iterator> it(db->NewIterator());
  for (it->Seek("user:"); it->Valid() && it->Key().rfind("user:", 0) == 0; it->Next()) {
    std::cout << "Scan: " << it->Key() << " => " << it->Value() << std::endl;
  }

  // 4. Delete a key (writes a tombstone marker)
  db->Delete("user:1001");

  delete db;
  return 0;
}
```

---

## Verification & Test Harness

Strata includes extensive correctness and stress tests:

### 1. Concurrency Stress Test (`tests/concurrency_test.cc`)
Spawns **8 reader threads** (4 doing random point lookups, 4 doing continuous range scans) and **1 continuous writer thread** hammering a tiny 4KB memtable. This triggers continuous minor flushes and major compactions while reader threads are actively traversing iterators and point queries.
- **Verification**: Zero data races under ThreadSanitizer (TSan), zero assertion failures, and consistent snapshot reads across 20,000+ operations.

### 2. Crash Recovery Test (`tests/crash_test.py`)
Launches a continuous writer process and delivers sudden `SIGKILL` (`kill -9`) signals mid-write across multiple iterations. Upon reopening the database, the test verifies:
1. Every write acknowledged prior to the crash is fully recovered.
2. Incomplete records at the tail of the WAL are detected and truncated.
3. No corrupted or partial state enters the memtable or version manifest.

### 3. Sanitizer Matrix
Strata runs continuously in CI under AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan), and ThreadSanitizer (TSan) across Linux (GCC and Clang) and macOS.

---

## Build Instructions

### Prerequisites
- C++17 compliant compiler (GCC 9+ or Clang 10+)
- CMake 3.15+
- Python 3 (for crash testing)

### Build Commands

```bash
# 1. Clone the repository
git clone https://github.com/geonna51/strata.git
cd strata

# 2. Configure and build Release binaries
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

# 3. Run all unit and integration tests
ctest --test-dir build --output-on-failure

# 4. Run the 8-reader concurrency stress test
./build/concurrency_test

# 5. Run the crash recovery test
python3 tests/crash_test.py

# 6. Run the benchmark harness
./build/db_bench --num=100000 --value_size=100
```

---

## Repository Structure

```
strata/
├── include/strata/          # Public C++17 API
│   ├── db.h                 # DB::Open, Put, Get, Delete, NewIterator
│   ├── iterator.h           # Scan iterator (Seek, Next, Key, Value)
│   ├── options.h            # Write buffer, cache size, sync configuration
│   ├── slice.h              # Zero-copy string view wrapper
│   └── status.h             # Result status representation (OK, NotFound, IOError)
├── src/                     # Core LSM-tree implementation
│   ├── skiplist.h           # Probabilistic lock-free reader SkipList
│   ├── memtable.h / .cc     # In-memory sorted memtable
│   ├── wal.h / .cc          # Append-only Write-Ahead Log with CRC32
│   ├── block.h / .cc        # 4KB block builder & reader with restart arrays
│   ├── bloom_filter.h / .cc # MurmurHash3 Bloom filter (10 bits/key)
│   ├── block_cache.h / .cc  # Concurrent LRU block cache
│   ├── sstable_builder.cc   # SSTable file writer
│   ├── sstable_reader.cc    # SSTable reader & block decoder
│   ├── version_set.h / .cc  # MANIFEST logger & version tracking
│   ├── compaction.h / .cc   # Multi-level compaction & directory sync
│   ├── db_impl.h / .cc      # Main database coordinator
│   └── env.h                # POSIX directory sync & durable fsync helpers
├── benchmarks/
│   └── db_bench.cc          # Latency percentile & throughput benchmark suite
├── tests/
│   ├── concurrency_test.cc  # 8-reader + 1-writer concurrency torture test
│   ├── crash_writer.cc      # Subprocess for crash testing
│   ├── crash_test.py        # kill -9 recovery test runner
│   ├── db_test.cc           # Full end-to-end functionality tests
│   ├── memtable_test.cc     # SkipList & Memtable unit tests
│   ├── sstable_test.cc      # SSTable format & block unit tests
│   ├── version_set_test.cc  # Manifest recovery unit tests
│   └── wal_test.cc          # WAL append & corruption recovery tests
├── docs/assets/             # Architecture, SSTable, and benchmark SVG diagrams
└── .github/workflows/
    └── ci.yml               # GitHub Actions CI matrix (Linux GCC/Clang, macOS, ASan/TSan)
```

---

## License

MIT License. Open source and free for educational and commercial use.
