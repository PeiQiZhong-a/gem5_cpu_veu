#!/usr/bin/env python3
"""Validate one generated RV+VEU E2E timing-model run."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path


STAT_PREFIX = "system.pipeline."


def fail(message: str) -> None:
    raise RuntimeError(message)


def read_stats(path: Path) -> dict[str, int]:
    result: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) < 2 or not fields[0].startswith(STAT_PREFIX):
            continue
        try:
            result[fields[0]] = int(float(fields[1]))
        except ValueError:
            continue
    return result


def stat(stats: dict[str, int], name: str) -> int:
    key = STAT_PREFIX + name
    if key not in stats:
        fail(f"missing stat {key}")
    return stats[key]


def last_retired_value(path: Path, register: str) -> int:
    expression = re.compile(rf"\brd={re.escape(register)}\s+data=(0x[0-9a-fA-F]+)")
    value: int | None = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        found = expression.search(line)
        if found:
            value = int(found.group(1), 0)
    if value is None:
        fail(f"missing retire trace for {register}")
    return value


def trace_events(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as input_file:
        return list(csv.DictReader(input_file))


def number(value: str) -> int:
    return int(value, 0)


def validate_trace(events: list[dict[str, str]], metadata: dict[str, object]) -> None:
    starts = [row for row in events if row["event"] == "operation_start"]
    finishes = [row for row in events if row["event"] == "operation_finish"]
    if len(starts) != 1 or len(finishes) != 1:
        fail(f"trace operation start/finish count is {len(starts)}/{len(finishes)}, expected 1/1")
    if starts[0]["op"] != metadata["op"]:
        fail(f"trace op {starts[0]['op']} does not match {metadata['op']}")

    read_requests = [row for row in events if row["event"] == "read_request"]
    write_requests = [row for row in events if row["event"] == "write_request"]
    if sorted(number(row["addr"]) for row in read_requests) != sorted(metadata["read_addresses"]):
        fail("read request addresses do not match the generated source chunks")
    if sorted(number(row["addr"]) for row in write_requests) != sorted(metadata["write_addresses"]):
        fail("write request addresses do not match the generated destination chunks")

    request_ids = {
        row["transaction_id"]
        for row in events
        if row["event"] in {"read_request", "write_request"}
    }
    response_ids = {
        row["transaction_id"]
        for row in events
        if row["event"] in {"read_response", "write_response"}
    }
    if request_ids != response_ids:
        fail("trace request and response transaction IDs do not match")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--stats", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--run-log", type=Path, required=True)
    args = parser.parse_args()

    metadata = json.loads(args.metadata.read_text(encoding="utf-8"))
    stats = read_stats(args.stats)
    mismatch = last_retired_value(args.run_log, "x10")
    if mismatch != 0:
        fail(f"RV result checker accumulated mismatch 0x{mismatch:08x}")

    expected = {
        "veu_operation_start_count": 1,
        "veu_operation_complete_count": 1,
        "veu_chunks": int(metadata["chunks"]),
        "veu_memory_reads": int(metadata["expected_reads"]),
        "veu_memory_writes": int(metadata["expected_writes"]),
        "veu_profile_hits": 1,
        "veu_profile_misses": 0,
        "veu_profile_fallbacks": 0,
        "veu_unexpected_responses": 0,
        "veu_illegal_operations": 0,
        "veu_current_outstanding_reads": 0,
    }
    actual = {name: stat(stats, name) for name in expected}
    for name, value in expected.items():
        if actual[name] != value:
            fail(f"{name} is {actual[name]}, expected {value}")
    if stat(stats, "cycle_count") >= int(metadata["max_cycles"]):
        fail("simulation reached max-cycles")
    if stat(stats, "veu_max_outstanding_reads") > 4:
        fail("outstanding read bound exceeded four")
    if stat(stats, "veu_retries") < 0:
        fail("retry count is negative")

    validate_trace(trace_events(args.trace), metadata)
    print(
        ",".join(
            (
                str(metadata["case"]),
                str(metadata["op"]),
                str(metadata["vlen"]),
                "PASS",
                str(stat(stats, "cycle_count")),
                str(actual["veu_chunks"]),
                str(actual["veu_memory_reads"]),
                str(actual["veu_memory_writes"]),
                str(stat(stats, "veu_max_outstanding_reads")),
                str(stat(stats, "veu_retries")),
                str(actual["veu_profile_hits"]),
                str(actual["veu_profile_fallbacks"]),
                "full-result-and-trace-checked",
            )
        )
    )


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"VEU E2E verification failed: {error}", file=sys.stderr)
        sys.exit(1)
