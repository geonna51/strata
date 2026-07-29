# Strata

[![Strata CI](https://github.com/geonna51/strata/actions/workflows/ci.yml/badge.svg)](https://github.com/geonna51/strata/actions/workflows/ci.yml)
[![C++17](https://img.shields.io/badge/standard-C%2B%2B17-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B17)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Dependencies](https://img.shields.io/badge/dependencies-zero-brightgreen.svg)]()

Strata is a high-performance, embedded Log-Structured Merge (LSM) tree Key-Value storage engine written in clean, modern C++17. Designed from first principles with **zero external dependencies**, Strata provides durable point lookups, range scans, concurrent read/compaction lifetimes, atomic crash recovery, and high-throughput write buffering.

---

### Key Architectural Highlights

- **Concurrent Read/Compaction Snapshot Isolation**: Readers pin active immutable versions (`std::shared_ptr<Version>`) and underlying SSTable file descriptors (`std::enable_shared_from_this<SSTableReader>`), guaranteeing complete safety and zero use-after-free conditions while background compactions unlink obsolete files on disk.
- **Power-Loss Durability**: Directory sync (`SyncDirectory`) and physical storage synchronization (`fcntl(F_FULLFSYNC)` / `fdatasync`) ensure crash-safe atomic pointer swaps (`CURRENT`) and metadata persistence across hardware cutoffs.
- **Zero External Dependencies**: Pure C++17 standard library and POSIX system calls. No Boost, no Abseil, no third-party libraries required.
- **Sub-Microsecond Lookups**: Blazing fast Bloom filter evaluation (~0.08 µs/op) and LRU block caching deliver over **7,000,000 read queries/sec**.

---

## Architecture

```mermaid
flowchart TD
    subgraph Client ["Client Interface"]
        API["API: Put() / Get() / Delete() / NewIterator()"]
    end

    subgraph Memory ["In-Memory Pipeline"]
        MT["Active Memtable<br/>(Lock-Free Concurrent SkipList)"]
        IMM["Immutable Memtable<br/>(Awaiting Minor Compaction)"]
    end

    subgraph DiskWAL ["Durable Write-Ahead Log"]
        WAL["WAL (Append-Only)<br/>IEEE 802.3 CRC32 Checksummed Frames"]
    end

    subgraph Storage ["On-Disk LSM Storage Hierarchy"]
        L0["Level 0: SSTables<br/>(Overlapping Key Ranges)"]
        L1["Level 1: SSTables<br/>(Partitioned, Non-Overlapping)"]
        L2["Level 2..6: SSTables<br/>(10x Multi-Level Compaction)"]
    end

    subgraph Versioning ["State &amp; Version Management"]
        VSET["VersionSet &amp; VersionEdit<br/>Atomic Directory Sync &amp; Snapshot Pinning"]
        CACHE["LRU Block Cache (4KB) &amp; Bloom Filters (10 bpk)"]
    end

    API -->|1. Write Record| WAL
    API -->|2. Insert Key-Value| MT
    MT -->|Buffer Full (4MB)| IMM
    IMM -->|Minor Compaction| L0
    L0 -->|File Count &gt;= 4| L1
    L1 -->|Size Triggered (10MB+)| L2

    API -->|Read: Check 1| MT
    API -->|Read: Check 2| IMM
    API -->|Read: Check 3| L0
    API -->|Read: Check 4| L1
    API -->|Read: Check 5| L2

    L0 -.-> CACHE
    L1 -.-> CACHE
    L2 -.-> CACHE
    VSET -.->|Tracks File Metadata| Storage
```

---

## SSTable Binary Format

SSTables are immutable on-disk files designed for fast sequential reads, random block indexing, and minimal disk seeks via Bloom filter early exits:

<p align="center">
  <img src="docs/assets/sstable-format.svg" alt="Strata SSTable Format" width="100%">
</p>

1. **Data Blocks (4KB)**: Entries are sorted by key. Keys use 16-entry prefix compression (`shared_len`, `non_shared_len`, `val_len`, delta bytes, 8-byte sequence/type tag, and value payload). Restart arrays at the tail of each block allow fast $O(\log K)$ binary search within blocks.
2. **Filter Block**: MurmurHash3-based Bloom filter bitset configured at 10 bits per key (~1% false positive probability). Prevents disk access when querying nonexistent keys.
3. **Index Block**: Maps separator keys to Data Block Handles (`offset` + `size`).
4. **Footer (48 Bytes Fixed)**: Located at the final 48 bytes of the file. Contains the Metaindex Block Handle (16 bytes), Index Block Handle (16 bytes), zero padding (8 bytes), and magic number `0xdb4772617461` (ASCII `"Grata"`).

---

## Benchmarks

Benchmarked using `benchmarks/db_bench.cc` on Apple Silicon (M2 Pro, 12 cores, 16 GB unified memory, macOS Darwin 25.6.0, Release build `-O3` with Clang 17) operating on 100,000 entries (16-byte keys, 100-byte values):

### Throughput (Operations / Second)

<p align="center">
  <img src="docs/assets/benchmark-throughput.svg" alt="Strata Throughput Benchmark" width="100%">
</p>

### Latency Distribution

<p align="center">
  <img src="docs/assets/benchmark-latency.svg" alt="Strata Latency Distribution" width="100%">
</p>

### Performance Summary Table

| Workload | Throughput | Bandwidth | p50 Latency | p95 Latency | p99 Latency | Notes |
| :--- | :---: | :---: | :---: | :---: | :---: | :--- |
| **`readmissing`** | **7,021,999 ops/s** | 107.1 MB/s | **0.08 µs** | 0.08 µs | 0.08 µs | 99%+ filtered by Bloom filter |
| **`readseq`** | **1,695,389 ops/s** | 184.3 MB/s | **0.59 µs** | 0.59 µs | 0.59 µs | Sequential multi-way iterator scan |
| **`readrandom`** (Warm) | **1,113,525 ops/s** | 121.1 MB/s | **0.83 µs** | 1.25 µs | 1.58 µs | Served from 64MB LRU block cache |
| **`readcold`** (Disk) | **866,685 ops/s** | 94.2 MB/s | **0.67 µs** | 1.08 µs | 13.62 µs | Cold cache, reads disk SSTable blocks |
| **`fillrandom`** | **134,213 ops/s** | 14.6 MB/s | **3.21 µs** | 4.50 µs | 7.71 µs | Random writes: WAL + SkipList Memtable |
| **`fillseq`** | **122,003 ops/s** | 13.3 MB/s | **3.12 µs** | 5.46 µs | 9.96 µs | Sequential writes: WAL + Memtable |
| **`fillsync`** | **238 ops/s** | 0.03 MB/s | **4.05 ms** | 5.03 ms | 5.52 ms | Physical drive flush (`F_FULLFSYNC`) per write |

---

## Concurrency & Lifetime Correctness

### Concurrent Reader / Compaction Isolation
LSM trees run background compaction threads that read existing SSTables, merge them into new files, and delete obsolete SSTables from disk. Strata avoids data races and dangling references using strict pointer pinning:
1. `DBImpl::Get()` and `DBImpl::NewIterator()` capture a `std::shared_ptr<Version>` while holding the database mutex, then release the mutex before reading or iterating.
2. `SSTableReader` inherits from `std::enable_shared_from_this<SSTableReader>`. When creating iterators, `reader->shared_from_this()` is pinned inside the iterator.
3. Because POSIX filesystems retain inodes for unlinked files as long as an open file descriptor remains active, obsolete SSTables unlinked by compaction remain completely valid and safe for active readers until the version is dropped.

### Concurrency Torture Test
The test suite includes `tests/concurrency_test.cc`:
- **8 Reader Threads**: 4 performing random point lookups, 4 performing continuous 20-step range scans.
- **1 Continuous Writer Thread**: Writing nonstop random updates.
- **Tiny 4KB Memtable**: Forces hundreds of memtable flushes and cascading multi-level compactions during live reads.
- **Result**: Zero data races, zero assertions, zero read errors across tens of thousands of operations.

---

## Power-Loss Durability & Directory Sync

Strata implements POSIX-level atomic commit and crash safety guarantees:

```
[Write New Manifest] -> fsync(MANIFEST) -> write(CURRENT.tmp) -> fsync(CURRENT.tmp)
                     -> rename(CURRENT.tmp, CURRENT) -> fsync(Database Directory)
```

1. **Atomic Pointer Swapping**: The `CURRENT` file points to the active `MANIFEST` descriptor log. Strata writes `CURRENT.tmp`, calls `fsync()` on the file descriptor, atomically `rename()`s it over `CURRENT`, and executes `SyncDirectory(dbname)` to flush the directory inode table to physical disk.
2. **Checksummed WAL**: The Write-Ahead Log encodes length, sequence number, record type, CRC32 checksum, key, and value. If power is lost mid-write, partial writes at EOF fail CRC32 verification and are safely discarded upon database reopen.
3. **Crash Recovery Test (`tests/crash_test.py`)**: Spawns an active database process, issues unannounced `SIGKILL` (`kill -9`) signals during active writes, and verifies 100% data integrity and consistency upon recovery.

---

## Quick Start

### Basic Usage Example

```cpp
#include <iostream>
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

  // Put a record
  s = db->Put("user:1001", "George");

  // Get a record
  std::string value;
  s = db->Get("user:1001", &value);
  if (s.ok()) {
    std::cout << "user:1001 => " << value << std::endl;
  }

  // Iterate over a range
  std::unique_ptr<strata::Iterator> it(db->NewIterator());
  for (it->Seek("user:"); it->Valid() && it->Key().rfind("user:", 0) == 0; it->Next()) {
    std::cout << it->Key() << " => " << it->Value() << std::endl;
  }

  // Delete a record (writes tombstone)
  db->Delete("user:1001");

  delete db;
  return 0;
}
```

---

## Building & Testing

### Prerequisites
- C++17 compatible compiler: GCC 9+ or Clang 10+
- CMake 3.15+
- Python 3.8+ (for crash testing)

### Build Commands

```bash
# Clone repository
git clone https://github.com/geonna51/strata.git
cd strata

# Build Release binaries
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

# Run full CTest suite
ctest --test-dir build --output-on-failure

# Run Concurrency Torture Test
./build/concurrency_test

# Run Crash & Recovery Test
python3 tests/crash_test.py

# Run Benchmark Suite
./build/db_bench --num=100000 --value_size=100
```

---

## Project Structure

```
strata/
├── .github/workflows/ci.yml # GitHub Actions CI (Linux GCC/Clang, macOS, ASan/TSan)
├── include/strata/          # Public API headers
│   ├── db.h                 # DB interface (Open, Put, Get, Delete, NewIterator)
│   ├── iterator.h           # Bidirectional scan iterator interface
│   ├── options.h            # Configurable runtime engine options
│   ├── slice.h              # Zero-copy string view wrapper
│   └── status.h             # Error handling & status codes
├── src/                     # Core storage engine implementation
│   ├── block.h / .cc        # 4KB binary data blocks with restart arrays
│   ├── block_cache.h / .cc  # O(1) concurrent LRU block cache
│   ├── bloom_filter.h / .cc # MurmurHash3 Bloom filter generator & tester
│   ├── compaction.h / .cc   # Background multi-level compaction & tombstone purging
│   ├── crc32.h / .cc        # IEEE 802.3 CRC32 checksum engine
│   ├── db_impl.h / .cc      # Core database coordinator & thread manager
│   ├── env.h                # POSIX directory sync and durable file sync utilities
│   ├── memtable.h / .cc     # Versioned in-memory memtable
│   ├── skiplist.h           # Probabilistic lock-free reader SkipList
│   ├── sstable_builder.cc   # SSTable binary file encoder
│   ├── sstable_reader.cc    # SSTable decoder, block reader & footer validator
│   ├── version_set.h / .cc  # MANIFEST logger, VersionEdit & MVCC version set
│   └── wal.h / .cc          # Append-only Write-Ahead Log writer & reader
├── benchmarks/
│   └── db_bench.cc          # Latency percentile & throughput benchmark harness
├── docs/
│   └── assets/              # SVG architectural diagrams & benchmark plots
└── tests/
    ├── concurrency_test.cc  # 8-reader + continuous writer torture test
    ├── crash_writer.cc      # Subprocess writer for kill -9 crash test
    ├── crash_test.py        # Crash recovery & corruption test runner
    ├── db_test.cc           # End-to-end database operations test
    ├── memtable_test.cc     # Memtable isolation & ordering tests
    ├── sstable_test.cc      # SSTable encoding & decoding verification
    ├── version_set_test.cc  # Manifest replay & delta edit tests
    └── wal_test.cc          # Write-ahead log corruption & repair tests
```

---

## License

This project is open source and available under the [MIT License](LICENSE).
