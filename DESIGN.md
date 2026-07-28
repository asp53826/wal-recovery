# Recovery contract

## Durability boundary

`commit(tx)` appends a commit record and calls `fsync` before returning success.
The acknowledgement is the public durability boundary. A caller may treat an
acknowledged transaction as committed; it may not infer commitment from a
successful `PUT` append.

## Atomicity boundary

Recovery buffers operations by transaction ID. It applies a transaction only
after reading a checksum-valid `COMMIT` record for that ID. Operations from
uncommitted, truncated, or corrupt transactions are discarded.

## Tail damage

The scanner stops at the first invalid record:

- incomplete header;
- wrong magic or unsupported version;
- unreasonable key/value length;
- incomplete payload or checksum;
- checksum mismatch;
- invalid record sequence.

Recovery reports the verified byte offset and can truncate the file to that
boundary. It never scans forward looking for another magic number because
accepting data after an unknown gap would invent an ordering guarantee.

## Crash-test oracle

The crash harness maintains an acknowledgement file separate from the WAL.
After a commit returns, the child process appends the committed transaction to
the oracle and syncs it. A parent kills the child at randomized instruction
boundaries, recovers the WAL, and verifies:

1. every oracle transaction is present;
2. no partially written transaction is visible;
3. the recovered state matches replaying the oracle prefix.

The oracle is intentionally conservative: a crash after WAL `fsync` but before
oracle `fsync` may leave an extra valid transaction in the WAL. The harness
records that as an ambiguous success window rather than data loss.

