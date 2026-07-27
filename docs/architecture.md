# Architecture

Strata is an embedded Key-Value store based on a Log-Structured Merge (LSM) tree architecture. All writes are converted into sequential disk appends, buffering updates in memory before flushing sorted runs to disk.

```
                     +---------------------------------------+
                     |             strata::DB                |
                     +---------------------------------------+
                        /                 |               \
                       / (1) Write        | (2) Insert     \ (3) Read
                      v                   v                 v
            +------------------+  +---------------+  +-------------------+
            | Write-Ahead Log  |  | Active Memtable|  | Immutable Memtable|
            |   (Append-Only)  |  |  (Skip List)  |  |    (Flushing)     |
            +------------------+  +---------------+  +-------------------+
                                                              |
                                                              v Flush
   +-------------------------------------------------------------------------+
   | Disk Storage                                                            |
   |                                                                         |
   | Level 0: [000003.sst] [000004.sst] [000005.sst] [000006.sst]            |
   |                                                                         |
   |             \                      /                                    |
   |              v Leveled Compaction v                                     |
   |                                                                         |
   | Level 1: [000007.sst: a-k]   [000008.sst: l-z]                          |
   |                                                                         |
   | MANIFEST: Records added and deleted files per level                     |
   +-------------------------------------------------------------------------+
```

---

## 1. Write Path

Every write operation (`Put` or `Delete`) executes with the following ordering:

1. **Write Buffer Capacity Check**: If the active Memtable exceeds `Options::write_buffer_size`, the writer initiates a flush:
   - The active Memtable is moved to the immutable slot (`imm_`).
   - A new empty active Memtable is created.
   - A new WAL file is opened.
   - The background worker is signaled to flush `imm_`.
2. **WAL Append**: The operation is assigned a monotonically increasing sequence number and appended to the current log file.
   - If `Options::sync` is `true`, `fsync()` / `fdatasync()` is executed before proceeding.
   - If `Options::sync` is `false`, the record is appended via unbuffered write syscalls and flushed asynchronously by the operating system page cache.
3. **Memtable Insert**: The record is inserted into the active SkipList. Deletions are stored as tombstones (`ValueType::kTypeDeletion`).
4. **Return Success**: The caller receives `Status::OK()`.

Writes never perform random disk I/O.

---

## 2. Read Path

A lookup (`Get`) executes without holding the database write mutex. It queries sources from newest to oldest:

```
[ Active Memtable ]
        | (not found)
        v
[ Immutable Memtable ]
        | (not found)
        v
[ Level 0 SSTables (newest to oldest) ]
        | (not found)
        v
[ Level 1..6 SSTables (binary search by key range) ]
```

At each stage:
- If a matching user key is found with `kTypeValue`, the value is copied to the caller and `Status::OK()` is returned.
- If a matching user key is found with `kTypeDeletion`, `Status::NotFound()` is returned immediately.
- When querying an SSTable:
  1. The Bloom filter is checked. If the filter returns false, disk blocks are never read.
  2. The Index block is binary-searched to find the candidate data block.
  3. The Block Cache is checked. If cached, the block is read from memory; otherwise, it is loaded from disk via `pread()`.
  4. The data block restart array is binary-searched to locate the record.

---

## 3. Compaction State Machine

### Minor Compaction (Flush)

Triggered when the active Memtable reaches capacity.
1. The Memtable is frozen into `imm_`.
2. A new Level 0 SSTable file is allocated (`<dbname>/<num>.sst`).
3. An `SSTableBuilder` writes entries sorted by key ascending, sequence number descending.
4. A `VersionEdit` records the addition of the new file at Level 0.
5. The edit is committed to the `MANIFEST`.
6. The obsolete WAL file corresponding to the flushed Memtable is unlinked.

### Major Compaction (Leveled)

Triggered when Level 0 has $\ge$ `Options::max_level0_files` (default: 4 files):

```
Level 0 (overlapping):  [ a - m ]  [ c - r ]  [ f - z ]
Key range: [ a - z ]
Overlapping Level 1:    [ b - g ]  [ h - t ]
```

1. **Input Selection**: All Level 0 files are selected. The bounding key range `[min_key, max_key]` is computed. Any Level 1 files overlapping this range are included.
2. **Multi-way Merge**: A `MergingIterator` merges all input iterators concurrently.
3. **Deduplication**:
   - For any key with multiple versions, only the newest version with the highest sequence number is retained. Older superseded versions are discarded.
   - Tombstones are evaluated via `Compaction::IsBaseLevelForKey`. If the key cannot exist in any deeper level (Level 2+), the tombstone is discarded. Otherwise, it is preserved in Level 1.
4. **Output Partitioning**: The merged stream is written to new Level 1 SSTables. When an output file reaches `Options::max_file_size` (default: 2MB), a new file is started.
5. **Atomic Manifest Commit**: A `VersionEdit` specifies the deletion of the input Level 0 and Level 1 files, and the addition of the new Level 1 files.
6. **File Unlinking**: Once the `MANIFEST` is persisted and the new `Version` is installed, the input files are unlinked from disk.

---

## 4. Concurrency and Reference Counting

- **Single-Writer, Concurrent-Reader**: Writes synchronize through `DBImpl::mutex_`. Point lookups and range scan iterators capture shared pointer references to the active `Memtable`, `imm_`, and the current `Version`.
- **Immutable SSTables**: SSTable files are never mutated after creation. When compaction finishes, files are replaced atomically at the metadata level in the `VersionSet`. Active iterators and readers keep their file descriptors valid via reference-counted handles until destroyed, preventing use-after-free or torn reads during compaction.
