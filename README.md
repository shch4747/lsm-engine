# lsm-engine

A persistent key-value storage engine in C++17, modelled on LevelDB/RocksDB.
Built from scratch across five modules: write-ahead log, memtable/SSTable flush,
Bloom-filtered read path, compaction, and a latency benchmark harness.

## Benchmark results (50,000 ops, 256KB memtable threshold)

| Workload | Throughput | p50 | p95 | p99 |
|---|---|---|---|---|
| Sequential write | 74,633 ops/sec | 7.6 µs | 13.4 µs | 64.4 µs |
| Random write | 63,009 ops/sec | 8.5 µs | 16.4 µs | 69.2 µs |
| Random read (70% hit) | 25,032 ops/sec | 39.8 µs | 94.9 µs | 217.1 µs |

**Compaction:** 8 SSTables → 97,543 keys kept, 1,891 stale/tombstone records
dropped in 4.82s.

**Why reads are slower than writes:** this is the LSM read-write asymmetry by
design. Writes always go to the in-memory memtable first (fast, sequential) and
never touch disk randomly. Reads must check the memtable then up to N SSTables
newest-first until the key is found — 8 SSTables in this benchmark. After
compaction those 8 collapse to 1, and read latency drops proportionally. This
is the fundamental tradeoff LSM trees make: optimize writes at the cost of read
amplification, then use compaction to periodically reclaim read performance.

## Architecture

```
put(k,v) ──▶ WAL append ──▶ memtable
                                │
                    (threshold exceeded)
                                │
                                ▼
                         SSTable flush ──▶ sst_N.sst
                         (sorted, immutable, indexed)
                                │
                    (N SSTables accumulated)
                                │
                                ▼
                           Compaction
                    (merge-sort, drop tombstones
                     and stale versions) ──▶ compacted.sst

get(k) ──▶ memtable ──▶ [Bloom filter] ──▶ SSTable_N ... SSTable_0
                 newest-first; Bloom filter skips SSTables that
                 definitely don't contain the key (zero false negatives)
```

## Modules

| # | File | What it does |
|---|---|---|
| 1 | `wal_store.hpp` | Write-ahead log: every mutation appended to disk before touching memory; `recover()` replays the log on startup to reconstruct exact state after a crash |
| 2 | `sstable.hpp` | `Memtable` (in-memory sorted map with flush trigger) + `SSTableWriter` (flush to immutable sorted file with binary-search index) + `SSTableReader` (point lookup via index) |
| 3 | `bloom.hpp` + `lsm.hpp` | Bloom filter for fast negative lookups; `LSMEngine` unifies WAL + memtable + SSTable read path (newest-first) with automatic flush |
| 4 | `compaction.hpp` | Merge N SSTables newest-first, keep the latest version of each key, drop tombstones and stale versions |
| 5 | `benchmark.cpp` | Throughput + p50/p95/p99 latency for sequential writes, random writes, and random reads |

## Key design decisions (interview-ready)

**Why WAL before memtable?** durability. If the process crashes after writing to
memory but before flushing to an SSTable, the WAL lets you reconstruct the lost
writes. Without it, any acknowledged write could be lost. The tradeoff: every
write pays an append cost (cheap — sequential disk I/O) to get the durability
guarantee.

**Why are SSTables immutable?** immutability eliminates random writes entirely.
Updates never modify existing files; they go to a new memtable entry that shadows
the old one. This keeps write throughput high regardless of dataset size, and
makes compaction a clean merge-sort rather than an in-place edit.

**Why a Bloom filter?** for a key that doesn't exist, a naive read would search
every SSTable and pay a disk seek per file. A Bloom filter answers "definitely
not here" in O(k) hash operations with zero false negatives, eliminating most
unnecessary disk reads. The ~2% false positive rate means occasional unnecessary
reads, which is acceptable.

**Why tombstones instead of immediate deletes?** an SSTable is immutable, so you
can't remove a key from it. Instead, a delete writes a tombstone record that
shadows older versions. Compaction is what actually removes the data — once the
tombstone reaches the bottom of the SST stack with no older version below it, it
and the original record are both dropped.

**flush() vs fsync().** the WAL uses `flush()` (pushes to the OS page cache) not
`fsync()` (forces to physical disk). A true crash-safe engine needs `fsync()`,
but it's ~10–100× slower. This is a known tradeoff — production engines
(LevelDB, RocksDB) make it configurable.

## Building and running

```bash
# compile and run tests
g++ -std=c++17 -O2 -Wall -o test_wal   test_wal.cpp   && ./test_wal
g++ -std=c++17 -O2 -Wall -o test_sst   test_sstable.cpp && ./test_sst
g++ -std=c++17 -O2 -Wall -o test_lsm   test_lsm.cpp   && ./test_lsm
g++ -std=c++17 -O2 -Wall -o test_compact test_compaction.cpp && ./test_compact

# benchmark
g++ -std=c++17 -O2 -Wall -o bench benchmark.cpp && ./bench 50000
```

Requires C++17 and a standard filesystem implementation (GCC 8+ / Clang 7+ /
MSVC 2017+). No external dependencies.
