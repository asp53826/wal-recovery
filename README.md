# wal-recovery

A dependency-free, crash-safe write-ahead log built from scratch in C++17.

[![CI](https://github.com/asp53826/wal-recovery/actions/workflows/ci.yml/badge.svg)](https://github.com/asp53826/wal-recovery/actions/workflows/ci.yml)

The project is designed around one falsifiable claim:

> Once a transaction's commit record has been acknowledged after `fsync`, recovery
> must reproduce it after a crash. Transactions without a durable commit record
> must never become visible.

## Verified properties

- `BEGIN`, `PUT`, `DELETE`, and `COMMIT` records with monotonic transaction IDs;
- CRC32-protected, length-delimited records detect torn and corrupted writes;
- multi-operation transactions become visible atomically during replay;
- recovery applies only transactions ending in a checksum-valid commit record;
- invalid tails can be truncated to the last verified record boundary;
- commit acknowledgement occurs only after the commit record is passed to
  `fsync`;
- a deterministic fault campaign sends `SIGKILL` during deliberately split
  writes and compares recovery with an independently synced oracle.

## Record format

Every record is self-delimiting and independently verifiable. Integer fields
are little-endian.

```text
+----------+---------+------+----------+-------+---------+-----------+
| magic[4] | version | type | reserved | tx_id | key_len | value_len |
+----------+---------+------+----------+-------+---------+-----------+
| key bytes | value bytes | CRC32(header + payload)                     |
+------------------------------------------------------------------------+
```

Recovery never searches forward for another magic number after corruption.
Doing so could silently accept records after an ordering gap. It stops at the
first invalid record and, in repair mode, truncates there.

## Build and verify

The only requirements are a C++17 compiler, POSIX file primitives, Make, and
Python 3 for the external crash orchestrator.

```bash
make test
make crash-test
make benchmark
```

`make test` includes an exhaustive byte-by-byte truncation test over a complete
transaction. `make crash-test` runs 200 process crashes by default; set
`CRASH_ROUNDS` to change the campaign length.

```bash
CRASH_ROUNDS=1000 make crash-test
```

## Measured baseline

Recorded on an Apple M2 Pro running macOS, using Apple Clang 17:

| Verification | Result |
|---|---:|
| Unit assertions | 375 passed |
| Randomized `SIGKILL` rounds | 200 |
| Acknowledged transactions at final high-water mark | 406 |
| Torn/corrupt tail observations | 154 |
| Acknowledged transactions lost | **0** |
| Partial transactions made visible | **0** |
| 1,000 transactions, four writes each | 16,127.9 tx/s |
| Keys recovered after benchmark | 4,000 |

The benchmark performs one `fsync` per transaction and is a local baseline, not
a cross-machine performance claim. The crash test models abrupt process death.
Durability across host power loss still depends on the operating system,
filesystem, storage hardware, and their `fsync` guarantees.

## Tools

```bash
# Recover and print summary/state without modifying the file
./build/wal_tool dump data.wal

# Append transactions forever; used by the crash orchestrator
./build/wal_tool workload data.wal acknowledgements.tsv

# Run a synchronous-commit benchmark
./build/wal_tool benchmark data.wal 1000 4
```

The implementation uses no database, serialization, checksum, or test
framework behind the interface. The record codec, CRC32, transaction state
machine, recovery scanner, repair path, and fault harness are all contained in
this repository.
