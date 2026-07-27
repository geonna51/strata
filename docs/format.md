# On-Disk File Formats

This document describes the binary formats used by Strata for the Write-Ahead Log (WAL), Sorted String Tables (SSTables), and the Version Manifest.

## Write-Ahead Log (WAL)

WAL files use the naming pattern `<dbname>/<number>.log`. Writes are appended sequentially.

### Record Layout

Each log record is encoded as follows:

```
+---------------+-------------------+---------------+-------------------+
|  crc32 (4B)   | payload_len (4B)  |   type (1B)   |   sequence (8B)   |
+---------------+-------------------+---------------+-------------------+
| key_len (4B)  |     key bytes     | val_len (4B)  |    val bytes      |
+---------------+-------------------+---------------+-------------------+
```

- `crc32`: IEEE 802.3 CRC32 checksum computed over all bytes following `payload_len`.
- `payload_len`: Total byte size of the payload (`1 + 8 + 4 + key_len + 4 + val_len`).
- `type`: `0x01` for `kTypeValue` (Put), `0x00` for `kTypeDeletion` (Delete / Tombstone).
- `sequence`: Monotonically increasing 64-bit unsigned integer assigned to the operation.
- `key_len`: 32-bit unsigned little-endian length of the key.
- `key`: Raw user key bytes.
- `val_len`: 32-bit unsigned little-endian length of the value (0 for deletions).
- `value`: Raw value bytes.

### Crash Semantics and Recovery

If a write is interrupted mid-record by process termination or power loss, the log reader detects either:
1. An incomplete 8-byte header at EOF, or
2. A payload shorter than `payload_len`, or
3. A CRC32 mismatch between the header checksum and the read payload.

During recovery, uncorrupted records prior to the torn write are replayed into memory. The torn partial record at EOF is truncated, preventing data corruption.

---

## Sorted String Tables (SSTables)

SSTable files use the naming pattern `<dbname>/<number>.sst`. They are immutable once written.

### File Layout

```
[ Data Block 0 ]
[ Data Block 1 ]
...
[ Data Block N ]
[ Filter Block ]
[ Index Block ]
[ Footer (48 bytes) ]
```

### Data Block Format

Each data block targets a size of 4096 bytes (configurable via `Options::block_size`).

```
+-------------------------------------------------------------+
| Entry 0                                                     |
| Entry 1                                                     |
| ...                                                         |
| Entry M                                                     |
+-------------------------------------------------------------+
| Restart[0] (4B)                                             |
| Restart[1] (4B)                                             |
| ...                                                         |
| Restart[K-1] (4B)                                           |
+-------------------------------------------------------------+
| Num Restarts (4B)                                           |
+-------------------------------------------------------------+
| CRC32 Trailer (4B)                                          |
+-------------------------------------------------------------+
```

Each entry in a data block is formatted as:
- `key_len (4B)`: Length of key
- `key bytes`: Key data
- `sequence (8B)`: Sequence number
- `type (1B)`: ValueType (`kTypeValue` or `kTypeDeletion`)
- `val_len (4B)`: Length of value
- `val bytes`: Value data

Restart points store byte offsets within the data block. They enable $O(\log K)$ binary search inside the block before linear scanning. Every physical block written to disk is immediately followed by a 4-byte CRC32 trailer.

### Filter Block Format

Contains a Bloom filter generated across all unique user keys stored in the SSTable.
- Bit array: $M$ bytes, where $M = \max(8, \lceil(N \times \text{bits\_per\_key}) / 8\rceil)$.
- Probe count: 1 byte at the end storing $k$ (the number of hash probes, typically 7 for 10 bits/key).
- Hash function: MurmurHash3 with double hashing ($h_i = h_1 + i \times h_2$).

### Index Block Format

The Index Block is formatted identically to a Data Block. Each entry represents one Data Block:
- Key: The largest user key present in that Data Block.
- Value: An encoded `BlockHandle` (`offset: 8B`, `size: 8B`).

### Footer Format

A fixed-size 48-byte trailer located at `file_size - 48`:

```
+---------------------+--------------------+----------------+----------------+
| filter_handle (16B) | index_handle (16B) |  padding (8B)  | magic (8B)     |
+---------------------+--------------------+----------------+----------------+
```

- `filter_handle`: `offset (8B)` and `size (8B)` of the Filter Block.
- `index_handle`: `offset (8B)` and `size (8B)` of the Index Block.
- `padding`: 8 zero bytes.
- `magic`: Fixed 64-bit constant `0x5354524154413031ULL` (ASCII string `"STRATA01"`).

---

## Persistent Manifest & CURRENT

The Manifest tracks the active file set and level assignments across database restarts.

### CURRENT File

A small text file located at `<dbname>/CURRENT`. It contains the filename of the active descriptor log:

```
MANIFEST-000002
```

### Manifest File Format

The file `<dbname>/MANIFEST-<number>` is written using the WAL format. Each record contains a serialized `VersionEdit` delta:

- Tag `1`: Comparator name
- Tag `2`: Log number
- Tag `3`: Next file number
- Tag `4`: Last sequence number
- Tag `5`: Deleted file (`level: 4B`, `file_number: 8B`)
- Tag `6`: New file (`level: 4B`, `file_number: 8B`, `file_size: 8B`, `smallest_key`, `largest_key`)
