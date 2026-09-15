# VeloxDB

An in-memory key-value engine written from scratch in C++, with a dual-index
storage layer, a hand-written query front end, and crash recovery through a
write-ahead log. Every performance number in this README is produced by the
benchmarking suite in this repo and can be reproduced with one command.

## Design

### Dual indexing

Each record lives in two indexes that are kept in sync on every write:

| Index | Structure | Used for | Cost |
| --- | --- | --- | --- |
| Point index | Separate-chaining hash map, djb2 hashing, power-of-two buckets, load-factor 0.75 growth | `SELECT` by key | O(1) average |
| Ordered index | B-Tree, minimum degree 32 (up to 63 keys and 64 children per node) | `RANGE` scans, ordered snapshot writes | O(log n) |

The B-Tree uses binary search inside each node and implements full CLRS
deletion (predecessor/successor promotion, sibling borrowing, node merging), so
it stays balanced under arbitrary delete patterns rather than being rebuilt.
The high fanout keeps a 100,000-record tree only 4 levels deep.

### Containers

The engine implements its own hash map, B-Tree, and growable array
(`DynamicArray<T>`) rather than using `std::unordered_map`, `std::map`, or
`std::vector`. It does use `std::string` for keys and values, and `std::fstream`
for file I/O.

### Query front end

A hand-written lexer scans the input line into tokens, handling single- and
double-quoted string literals, and a dispatcher executes the statement:

```
[input line] -> [lexer] -> [DynamicArray<Token>] -> [dispatch] -> [indexes + WAL]
```

### Durability

Two files back the engine:

* `velox_store.bin` — length-prefixed binary snapshot, written in key order,
  with a magic header. Written to a temp file and renamed into place so an
  interrupted `SAVE` cannot corrupt the previous snapshot.
* `velox_store.wal` — append-only write-ahead log. Every `INSERT` and `DELETE`
  is appended and flushed *before* the write is acknowledged.

On startup the engine loads the snapshot and replays the log on top of it, so a
process killed mid-session comes back with all acknowledged writes. A truncated
final log record (from a process killed mid-write) is detected and discarded.
`SAVE` writes a fresh snapshot and compacts the log to zero bytes.

Note on scope: the log is flushed to the OS on every write, which survives a
process crash. It is not `fsync`ed, so it does not claim to survive a power loss.

## Commands

| Command | Description |
| --- | --- |
| `INSERT <key> "<value>"` | Write to both indexes, logged first |
| `SELECT <key>` | Point lookup through the hash index |
| `DELETE <key>` | Remove from both indexes, logged first |
| `RANGE <start> <end>` | Ordered scan through the B-Tree, inclusive |
| `SAVE` | Write a snapshot and compact the log |
| `STATS` | Record count, bucket count, load factor, B-Tree height |
| `EXIT` | Quit |

## Build and run

No third-party dependencies:

```bash
g++ -O3 -std=c++14 -Wall -Wextra veloxdb_core.cpp -o veloxdb_core
./veloxdb_core
```

```
veloxdb_engine# INSERT candidate101 "Rishit Bajaj - Systems Engineer"
[SYSTEM]: Committed to hash and B-Tree indexes, logged for recovery.

veloxdb_engine# SELECT candidate101
[VALUE]: Rishit Bajaj - Systems Engineer (hash index, under clock resolution; run --bench for calibrated latency)

veloxdb_engine# RANGE candidate100 candidate200
[RANGE]: 1 record(s) in [candidate100, candidate200]
  candidate101 => Rishit Bajaj - Systems Engineer

veloxdb_engine# STATS
[STATS]: records=1 hash_buckets=1024 hash_load_factor=0.001 btree_records=1 btree_height=1
```

The binary also runs headless for benchmarking: `--bench N` for the index
microbenchmark, `--workload N` for the insert/lookup workload, `--quiet` to
suppress the banner and prompt.

## Benchmarks

```bash
python3 benchmark.py                       # index sweep; MySQL if one is reachable
python3 benchmark.py --skip-mysql          # index sweep only
python3 benchmark.py --mysql-host 127.0.0.1 --mysql-user root --mysql-password secret
python3 benchmark.py --json results.json   # machine-readable output
```

### Index sweep

Lookup latency per structure over an identical probe sequence. Measured on
Windows, MinGW GCC 6.3, `-O3`:

| Records | Hash | B-Tree | Sorted array (binary search) | Flat array (linear scan) | B-Tree height |
| --- | --- | --- | --- | --- | --- |
| 1,000 | 15.8 ns | 79.7 ns | 81.4 ns | 1.67 µs | 2 |
| 10,000 | 20.4 ns | 176.2 ns | 157.9 ns | 21.01 µs | 3 |
| 100,000 | 16.0 ns | 240.7 ns | 264.7 ns | 203.52 µs | 4 |

Hash lookup latency stays flat at roughly 16–20 ns as the dataset grows 100x,
which is the O(1) claim holding up. Against flat array storage the indexed
lookup is 99.1% to 99.99% faster (106x to 12,731x); against a sorted array with
binary search it is 81% to 94% faster.

Reproduce with `python3 benchmark.py --skip-mysql`. Absolute numbers depend on
the machine; the scaling behaviour is the point.

### Head-to-head vs MySQL

The suite runs the same workload — N single-row inserts followed by N point
lookups by key — against VeloxDB and against a live MySQL server, and reports
latency and throughput side by side. It needs `pymysql` or
`mysql-connector-python` plus a reachable server:

```bash
pip install pymysql
mysql -u root -e "CREATE DATABASE IF NOT EXISTS veloxdb_bench"
python3 benchmark.py --records 20000 --mysql-user root --mysql-password secret
```

Measured against MySQL 8.0.46 (InnoDB) over TCP loopback, 20,000 records:

| Engine | Insert µs/op | Insert ops/s | Select µs/op | Select ops/s |
| --- | --- | --- | --- | --- |
| VeloxDB | 9.84 | 101,606 | 1.18 | 848,428 |
| MySQL, matched durability | 274.94 | 3,637 | 183.85 | 5,439 |
| MySQL, default (fsync per commit) | 13,933.48 | 72 | 213.28 | 4,689 |

Two MySQL rows are reported because durability guarantees have to match before
a latency comparison means anything. VeloxDB flushes its write-ahead log to the
OS on every write; MySQL's default additionally `fsync`s to physical media on
every commit, which is a stronger guarantee and costs roughly 50x on this
machine. The matched-durability row sets `innodb_flush_log_at_trx_commit=2` and
`sync_binlog=0` to line MySQL up with what VeloxDB actually promises, and is the
honest comparison: VeloxDB inserts 27.9x faster and looks up 155.9x faster.

Even at matched durability this is not an apples-to-apples fight. MySQL pays for
SQL parsing and a client/server round trip that an embedded in-process store
does not, which is most of the remaining lookup gap. The comparison measures the
cost of those guarantees, not a defect in MySQL.

If no driver or server is available the suite says so explicitly and reports the
VeloxDB side alone rather than inventing a comparison.
