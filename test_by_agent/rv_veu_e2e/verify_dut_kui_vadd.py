#!/usr/bin/env python3
"""Check dut_kui VADD memory timing and compare it with the fixed2 RTL run."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path


def fail(message: str) -> None:
    raise RuntimeError(message)


def load_trace(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as input_file:
        return list(csv.DictReader(input_file))


def one_cycle(events: list[dict[str, str]], event: str) -> int:
    matches = [int(row["cycle"], 0) for row in events if row["event"] == event]
    if len(matches) != 1:
        fail(f"expected one {event} event, found {len(matches)}")
    return matches[0]


def response_deltas(
    events: list[dict[str, str]], request_event: str, response_event: str,
) -> list[int]:
    requests = {
        row["transaction_id"]: int(row["cycle"], 0)
        for row in events if row["event"] == request_event
    }
    responses = {
        row["transaction_id"]: int(row["cycle"], 0)
        for row in events if row["event"] == response_event
    }
    if requests.keys() != responses.keys():
        fail(f"{request_event}/{response_event} transaction IDs differ")
    return [responses[transaction] - cycle for transaction, cycle in requests.items()]


def rtl_row(path: Path, vlen: int) -> dict[str, str]:
    with path.open(newline="", encoding="utf-8") as input_file:
        rows = list(csv.DictReader(input_file))
    matches = [
        row for row in rows
        if row["op"] == "vadd" and row["config"].lower() == "00000700"
        and int(row["vlen"]) == vlen
    ]
    if len(matches) != 1:
        fail(f"expected one RTL vector VADD VLEN={vlen}, found {len(matches)}")
    return matches[0]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--rtl-summary", type=Path, required=True)
    args = parser.parse_args()

    metadata = json.loads(args.metadata.read_text(encoding="utf-8"))
    if metadata["case"] != "vadd_vector" or int(metadata["config"]) != 0x700:
        fail("dut_kui timing check requires vector VADD config 0x700")

    events = load_trace(args.trace)
    start = one_cycle(events, "operation_start")
    finish = one_cycle(events, "operation_finish")
    lock_finish = one_cycle(events, "lock_finish")
    read_requests = [row for row in events if row["event"] == "read_request"]
    write_requests = [row for row in events if row["event"] == "write_request"]
    read_deltas = response_deltas(events, "read_request", "read_response")
    write_deltas = response_deltas(events, "write_request", "write_response")

    if any(delta != 4 for delta in read_deltas):
        fail(f"VEU read return deltas are {read_deltas}, expected all 4")
    if any(delta != 1 for delta in write_deltas):
        fail(f"VEU write completion deltas are {write_deltas}, expected all 1")

    first_read = min(int(row["cycle"], 0) for row in read_requests) - start
    first_write = min(int(row["cycle"], 0) for row in write_requests) - start
    rtl = rtl_row(args.rtl_summary, int(metadata["vlen"]))

    print(
        ",".join(
            (
                str(metadata["vlen"]),
                "PASS",
                str(first_read),
                rtl["first_read_delay"],
                ";".join(map(str, read_deltas)),
                rtl["read_return_pattern"],
                str(first_write),
                rtl["first_write_latency"],
                str(lock_finish - start),
                rtl["total_cycles"],
                str(finish - start),
                rtl["total_cycles"],
            )
        )
    )


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"dut_kui VADD verification failed: {error}", file=sys.stderr)
        sys.exit(1)
