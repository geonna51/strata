# Strata

Strata is an embedded key-value store based on a Log-Structured Merge (LSM) tree. It buffers writes in memory and flushes them to sorted string tables on disk, merging them in the background. Keys and values are arbitrary byte strings.

## Quick Example

```cpp
#include <iostream>
#include <string>
#include "strata/db.h"

int main() {
  strata::Options options;
  options.create_if_missing = true;

  strata::DB* db = nullptr;
  strata::Status s = strata::DB::Open(options, "/tmp/testdb", &db);
  if (!s.ok()) {
    std::cerr << "Open failed: " << s.ToString() << "\n";
    return 1;
  }

  s = db->Put("user:100", "Alice");
  std::string value;
  s = db->Get("user:100", &value);
  std::cout << "user:100 -> " << value << "\n";

  s = db->Delete("user:100");
  s = db->Get("user:100", &value);
  if (s.IsNotFound()) {
    std::cout << "Record deleted successfully.\n";
  }

  delete db;
  return 0;
}
```

## Architecture

```
User -> Put() ---+---> Active Memtable (In-Memory SkipList)
                 |
                 +---> Write-Ahead Log (WAL on Disk)

When Memtable reaches capacity -> Frozen to Immutable Memtable -> Flushed to Level 0 SSTable.
When Level 0 file count >= 4   -> Leveled Compaction merges files into Level 1 SSTables.
```

The database consists of four primary components:

- **Memtable**: An in-memory SkipList ordered by user key ascending and sequence number descending. Reads and writes check this structure first.
- **Write-Ahead Log (WAL)**: An append-only file on disk. Every mutation is written to the log before insertion into the Memtable. Each record includes an IEEE 802.3 CRC32 checksum to detect partial writes on crash.
- **SSTables (Sorted String Tables)**: Immutable disk files structured in 4KB data blocks, a Bloom filter block, an index block, and a 48-byte footer. Binary search on the index locates the target data block, while Bloom filters avoid disk lookups for absent keys.
- **Compaction**: A background thread that flushes immutable memtables to Level 0 and merges overlapping Level 0 files into sorted, non-overlapping Level 1 files, discarding overwritten keys and expired tombstones.

## Crash Recovery

If the application or operating system terminates unexpectedly during a write:

1. Records successfully written and synced to the WAL before termination are durable.
2. If termination occurs during an incomplete write, the trailing partial record at EOF fails CRC32 verification and is truncated.
3. Upon restart, `DB::Open` reads the `MANIFEST` file to restore the SSTable file set across all levels, then replays uncommitted WAL records into a new Memtable.
4. Data loss for all acknowledged synchronous writes is 0.

### Durability Guarantee: Synchronous vs Asynchronous Writes

By default, Strata uses asynchronous WAL writes (`options.sync = false`). Writes are handed to the operating system buffer cache via the `write()` system call, offering higher throughput. However, if the machine suffers a power loss before the OS flushes dirty pages to the storage drive, recent writes may be lost.

For applications requiring strict durability across power loss, setting `options.sync = true` forces an `fsync()` / `F_FULLFSYNC` call on every write before returning success.

The repository includes a crash torture test (`tests/crash_test.py`) that spawns an active writer process and issues `SIGKILL` (`kill -9`) mid-write, verifying that every acknowledged write is preserved upon recovery.

## Building

Strata requires a C++17 compiler (GCC 9+ or Clang 10+) and CMake 3.15+. It has zero external dependencies.

```bash
# Clone the repository
git clone https://github.com/georgen/strata.git
cd strata

# Configure and build Release binaries
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j4

# Run test suite
ctest --output-on-failure
```

To run the crash recovery test:

```bash
python3 ../tests/crash_test.py
```

## Benchmarks

Benchmarks were measured using `benchmarks/db_bench.cc`.

### Environment

- **Hardware**: Apple M2 Pro (12 CPU cores, 16 GB unified memory)
- **Operating System**: macOS Darwin 25.6.0
- **Build Mode**: Release (`-O3`, Clang 17)
- **Dataset**: 100,000 entries (Key size: 16 bytes, Value size: 100 bytes)
- **Block Size**: 4096 bytes
- **Block Cache**: 64 MB

### Measured Results

```
Workload        Throughput (ops/sec)    Throughput (MB/s)    Latency (us/op)
----------------------------------------------------------------------------
fillseq               147,808 ops/sec           16.1 MB/s         6.77 us/op
fillrandom            157,548 ops/sec           17.1 MB/s         6.35 us/op
readseq             1,825,814 ops/sec          198.5 MB/s         0.55 us/op
readrandom          1,224,960 ops/sec          133.2 MB/s         0.82 us/op
readmissing         5,116,659 ops/sec           78.1 MB/s         0.20 us/op
fillsync                  223 ops/sec            0.0 MB/s      4465.71 us/op
```

- **Asynchronous writes (`fillseq`, `fillrandom`)**: Average ~150,000 ops/sec with sub-7 microsecond latency by appending to the WAL and in-memory SkipList.
- **Synchronous writes (`fillsync`)**: Drops to ~223 ops/sec due to drive platter/NAND flush latency on each write (`fsync`).
- **Missing key reads (`readmissing`)**: Achieves >5,000,000 ops/sec via Bloom filter rejection without touching disk blocks.

To run benchmarks locally:

```bash
./build/db_bench --num=100000 --value_size=100
```

## Limitations

- **Embedded Only**: Strata is an embedded library linked into application processes. It does not provide a network daemon, REST API, or RPC interface.
- **Single Process**: Only one process may open a database directory at a time. Concurrency across processes is locked via file descriptors (`LOCK`).
- **No Compression**: Blocks are currently uncompressed.
- **No Column Families**: Keys share a single flat namespace.
- **POSIX Platform**: Designed and tested on POSIX systems (Linux and macOS).
