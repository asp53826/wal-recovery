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
- validated group commit amortizes one durability barrier across transactions;
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
| Unit assertions | **412 passed** |
| Randomized `SIGKILL` rounds | 200 |
| Acknowledged transactions at final high-water mark | 406 |
| Torn/corrupt tail observations | 154 |
| Acknowledged transactions lost | **0** |
| Partial transactions made visible | **0** |
| Single commit, 1,000 transactions × four writes | 15,577 tx/s |
| Group commit, 16 transactions per `fsync` | **40,162 tx/s** |
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

# Run single and grouped synchronous-commit benchmarks
./build/wal_tool benchmark data.wal 1000 4 1
./build/wal_tool benchmark grouped.wal 1000 4 16
```

The implementation uses no database, serialization, checksum, or test
framework behind the interface. The record codec, CRC32, transaction state
machine, recovery scanner, repair path, and fault harness are all contained in
this repository.

## Transaction state machine

The binary format alone does not make a transaction valid. Recovery enforces
the legal sequence:

```text
                 PUT / DELETE
              +----------------+
              |                v
BEGIN ------> active --------> active ------> COMMIT ------> visible
                 |
                 +------ clean EOF / damaged tail ------> discarded
```

An operation without `BEGIN`, a duplicate `BEGIN`, a second `COMMIT`, payload
bytes on control records, or a value attached to `DELETE` terminates the
verified prefix. Pending operations are applied only when the commit record
passes its CRC and sequence checks.

A 100-operation transaction therefore has exactly two recovery outcomes: all
100 operations or none. There is no state where the first 63 fit before a torn
write and became visible.

## Group commit

`fsync` once per transaction is simple, but the barrier dominates small
transactions. `commit_batch` validates every ID and rejects duplicates before
writing any commit record. It then appends all commit records and performs one
`fsync`.

```cpp
std::vector<std::uint64_t> ready;
for (const Request& request : batch) {
  auto tx = log.begin();
  log.put(tx, request.key, request.value);
  ready.push_back(tx);
}
log.commit_batch(ready);
```

Each transaction remains independent; they only share the durability barrier.

| commit policy | barriers | throughput | relative |
|---|---:|---:|---:|
| one transaction/barrier | 1,000 | 15,577 tx/s | 1.00× |
| 16 transactions/barrier | 63 | **40,162 tx/s** | **2.58×** |

The tradeoff is queueing latency: the first request waits for the rest of its
group. A production coordinator would cap both group size and wait time.

## Recovery policy

The scanner tracks `file_bytes` from the filesystem and `valid_bytes` at the
last complete, legal, checksum-valid record. On an incomplete header,
impossible length, CRC mismatch or illegal transition, recovery stops.

It never searches forward for another `WAL1` magic value. Finding magic later
would not prove that the gap was uncommitted or preserve ordering. Repair mode
truncates to `valid_bytes` and syncs the shorter file before append resumes.

Clean EOF after a complete `PUT` is different from damage. It leaves a valid
but uncommitted transaction, which is discarded without truncating legitimate
records.

## Why the external oracle matters

A kill test that merely checks recovery does not crash is weak: losing the
last hundred transactions would pass. The campaign keeps a second append-only
acknowledgement file. Only after WAL commit returns does the child append and
sync an oracle line.

After every random `SIGKILL`, the parent requires:

1. every complete oracle line exists in recovered state;
2. every recovered key/value pair is internally consistent;
3. no partial transaction becomes visible;
4. the recovery summary is parseable.

There is an unavoidable window after WAL `fsync` and before oracle `fsync`.
A transaction found only in the WAL is an ambiguous committed success, not a
loss. Incomplete oracle lines are truncated before restart.

`WAL_SPLIT_WRITES=1` emits each record in seven-byte chunks with delays, making
kills land inside headers, payloads and CRC fields.

## Tests that matter

- CRC32 matches the standard `123456789 → 0xCBF43926` vector;
- committed multi-operation transactions replay atomically;
- uncommitted transactions remain invisible at clean EOF;
- deletes replay and remove prior state;
- transaction IDs continue monotonically after restart;
- every byte truncation of a transaction preserves the prior commit and
  exposes none of the cut transaction;
- a flipped byte prevents the damaged transaction from replaying;
- 32 transactions committed through one barrier all recover;
- a group containing one invalid ID appends zero commit records;
- inactive transaction operations are rejected.

The every-byte truncation loop exercises headers, keys, values, record
boundaries and the commit marker—not just one convenient checksum cut.

## Code map

```text
include/wal/wal.h        transaction API and recovery result
src/wal.cpp              codec, CRC32, state machine, repair and group commit
src/main.cpp             dump tool, crash workload, oracle and benchmark
tests/test_wal.cpp        deterministic and exhaustive truncation tests
scripts/crash_torture.py process-kill orchestrator and independent verifier
.github/workflows/ci.yml macOS/Linux test and crash matrix
```

## Deliberate limitations

- one writer process and no inter-process lock;
- no checkpoints or WAL segment rotation;
- no direct or asynchronous I/O;
- CRC32 detects accidental corruption, not adversarial modification;
- `fsync` guarantees depend on the OS, filesystem and drive firmware;
- group commit is caller formed, not a timed background coordinator.

These are the next engineering layers, not features implied by “production
ready.”
