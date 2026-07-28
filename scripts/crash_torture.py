#!/usr/bin/env python3
"""Crash a WAL writer at random points and verify recovery invariants."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import random
import signal
import subprocess
import tempfile
import time


def parse_dump(binary: Path, wal_path: Path) -> tuple[dict[str, int], dict[str, str]]:
    completed = subprocess.run(
        [str(binary), "dump", str(wal_path)],
        check=True,
        capture_output=True,
        text=True,
    )
    summary: dict[str, int] | None = None
    state: dict[str, str] = {}
    for line in completed.stdout.splitlines():
        fields = line.split("\t")
        if fields[0] == "SUMMARY" and len(fields) == 7:
            names = (
                "valid_bytes",
                "file_bytes",
                "valid_records",
                "committed_transactions",
                "discarded_transactions",
                "tail_damaged",
            )
            summary = {name: int(value) for name, value in zip(names, fields[1:])}
        elif fields[0] == "KV" and len(fields) == 3:
            state[fields[1]] = fields[2]
        else:
            raise AssertionError(f"unparseable dump line: {line!r}")
    if summary is None:
        raise AssertionError("dump did not emit a summary")
    return summary, state


def read_oracle(path: Path) -> dict[str, str]:
    if not path.exists():
        return {}
    expected: dict[str, str] = {}
    with path.open("rb") as handle:
        for raw_line in handle:
            if not raw_line.endswith(b"\n"):
                break
            fields = raw_line.decode("utf-8").removesuffix("\n").split("\t")
            if len(fields) != 3:
                raise AssertionError(f"invalid oracle record: {raw_line!r}")
            transaction_id, key, value = fields
            if key != f"tx-{transaction_id}" or value != f"value-{transaction_id}":
                raise AssertionError(f"invalid oracle contents: {raw_line!r}")
            expected[key] = value
    return expected


def validate_state(recovered: dict[str, str], expected: dict[str, str]) -> int:
    for key, value in expected.items():
        if recovered.get(key) != value:
            raise AssertionError(
                f"acknowledged transaction lost: {key}={value!r}, "
                f"recovered={recovered.get(key)!r}"
            )

    for key, value in recovered.items():
        if not key.startswith("tx-") or not key[3:].isdigit():
            raise AssertionError(f"malformed recovered key: {key!r}")
        transaction_id = key[3:]
        if value != f"value-{transaction_id}":
            raise AssertionError(
                f"partially/corruptly recovered value: {key}={value!r}"
            )
    return len(set(recovered) - set(expected))


def run_campaign(binary: Path, rounds: int, seed: int) -> dict[str, int | float]:
    rng = random.Random(seed)
    started = time.monotonic()
    tail_damage_events = 0
    acknowledged_high_water = 0
    recovered_high_water = 0
    ambiguous_high_water = 0

    with tempfile.TemporaryDirectory(prefix="wal-crash-torture-") as directory:
        root = Path(directory)
        wal_path = root / "campaign.wal"
        oracle_path = root / "acknowledged.tsv"
        environment = os.environ.copy()
        environment["WAL_SPLIT_WRITES"] = "1"

        for crash_index in range(rounds):
            process = subprocess.Popen(
                [
                    str(binary),
                    "workload",
                    str(wal_path),
                    str(oracle_path),
                    str(rng.randint(0, 500)),
                ],
                env=environment,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
            )
            time.sleep(rng.uniform(0.001, 0.018))
            if process.poll() is not None:
                stderr = process.stderr.read() if process.stderr else ""
                raise AssertionError(
                    f"writer exited before crash {crash_index}: "
                    f"code={process.returncode}, stderr={stderr!r}"
                )
            process.send_signal(signal.SIGKILL)
            process.wait(timeout=5)
            if process.returncode != -signal.SIGKILL:
                raise AssertionError(
                    f"writer was not killed as expected: {process.returncode}"
                )

            summary, recovered = parse_dump(binary, wal_path)
            expected = read_oracle(oracle_path)
            ambiguous = validate_state(recovered, expected)

            tail_damage_events += summary["tail_damaged"]
            acknowledged_high_water = max(acknowledged_high_water, len(expected))
            recovered_high_water = max(recovered_high_water, len(recovered))
            ambiguous_high_water = max(ambiguous_high_water, ambiguous)

    return {
        "rounds": rounds,
        "seed": seed,
        "acknowledged_transactions": acknowledged_high_water,
        "recovered_transactions": recovered_high_water,
        "ambiguous_commit_window": ambiguous_high_water,
        "tail_damage_events": tail_damage_events,
        "acknowledged_losses": 0,
        "partial_transactions_visible": 0,
        "elapsed_seconds": round(time.monotonic() - started, 3),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--rounds", type=int, default=200)
    parser.add_argument("--seed", type=int, default=20260728)
    args = parser.parse_args()

    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"binary does not exist: {binary}")
    if args.rounds < 1:
        parser.error("--rounds must be positive")

    result = run_campaign(binary, args.rounds, args.seed)
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
