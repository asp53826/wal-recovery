# wal-recovery

A crash-safe write-ahead log built from scratch in C++17.

The project is designed around one falsifiable claim:

> Once a transaction's commit record has been acknowledged after `fsync`, recovery
> must reproduce it after a crash. Transactions without a durable commit record
> must never become visible.

## What this repository will prove

- length-delimited, checksummed binary records detect torn and corrupted writes;
- multi-operation transactions become visible atomically;
- recovery replays only transactions with a valid commit record;
- an invalid tail is truncated to the last verified record boundary;
- a kill-at-random-points harness repeatedly crashes a writer and compares the
  recovered state with an independently persisted acknowledgement oracle.

## Record format

Every record is self-delimiting and independently verifiable:

```text
+----------------+---------+------+--------+---------+-----------+----------+
| magic (4 bytes)| version | type | tx id  | key len | value len | payload  |
+----------------+---------+------+--------+---------+-----------+----------+
| CRC32 over header fields and payload (4 bytes)                           |
+----------------------------------------------------------------------------+
```

The first implementation milestone targets:

1. `BEGIN`, `PUT`, `DELETE`, and `COMMIT` records;
2. `fsync`-backed commit acknowledgement;
3. deterministic replay into an in-memory key/value state;
4. checksum, truncation, and partial-record tests;
5. a reproducible crash-torture benchmark.

## Build target

The code intentionally uses only the C++17 standard library and POSIX file
primitives available on macOS and Linux. No database library is hidden behind
the interface.

```bash
make test
make crash-test
```

Implementation is in progress. Measured recovery and crash-injection results
will replace this note when the first complete milestone is verified.

