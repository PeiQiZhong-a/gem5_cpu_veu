#!/usr/bin/env python3
"""Compare canonical Spirit RTL and gem5 cycle traces.

The comparison is intentionally layered but strict:
  1. reset/active edge numbering and DONE edge/value,
  2. retire sequence, retire edge, and normalized writeback,
  3. edge-by-edge CPU bus/control signals.

Only meaningful payload fields are compared. For example, IBus read data is
ignored unless ibus_resp is asserted and redirect_target is ignored unless a
redirect is active.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


TRACE_HEADER = "brs-cycle-trace-v1"
INT_RE = re.compile(r"^[+-]?[0-9]+$")
HEX_RE = re.compile(r"^0x[0-9a-f]+$")


def parse_value(value: str) -> int | str:
    value = value.lower()
    if HEX_RE.fullmatch(value):
        return int(value, 16)
    if INT_RE.fullmatch(value):
        return int(value, 10)
    return value


@dataclass(frozen=True)
class Trace:
    path: Path
    source: str
    records: tuple[dict[str, Any], ...]

    @property
    def by_edge(self) -> dict[int, dict[str, Any]]:
        return {record["edge"]: record for record in self.records}

    @property
    def active(self) -> tuple[dict[str, Any], ...]:
        return tuple(record for record in self.records
                     if record.get("reset") == 0)

    @property
    def retires(self) -> tuple[dict[str, Any], ...]:
        return tuple(record for record in self.active
                     if record.get("retire") == 1)

    @property
    def done(self) -> dict[str, Any] | None:
        return next((record for record in self.active
                     if record.get("done") == 1), None)


def load_trace(path: Path) -> Trace:
    records: list[dict[str, Any]] = []
    source = "unknown"
    saw_header = False
    previous_edge = 0

    with path.open("r", encoding="utf-8") as trace_file:
        for line_number, raw_line in enumerate(trace_file, 1):
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith("#"):
                if TRACE_HEADER in line:
                    saw_header = True
                    for token in line[1:].split():
                        if token.startswith("source="):
                            source = token.split("=", 1)[1]
                continue

            fields: dict[str, Any] = {}
            for token in line.split():
                if "=" not in token:
                    continue
                key, value = token.split("=", 1)
                fields[key] = parse_value(value)
            if "edge" not in fields:
                continue
            edge = fields["edge"]
            if not isinstance(edge, int):
                raise ValueError(f"{path}:{line_number}: invalid edge {edge!r}")
            if edge <= previous_edge:
                raise ValueError(
                    f"{path}:{line_number}: edge {edge} is not strictly "
                    f"after {previous_edge}")
            if "reset" not in fields or "cpu_cycle" not in fields:
                raise ValueError(
                    f"{path}:{line_number}: missing reset/cpu_cycle")
            previous_edge = edge
            records.append(fields)

    if not saw_header:
        raise ValueError(f"{path}: missing '# {TRACE_HEADER}' header")
    if not records:
        raise ValueError(f"{path}: no cycle records")
    return Trace(path, source, tuple(records))


def normalized_retire(record: dict[str, Any]) -> tuple[Any, ...]:
    wb_we = record.get("wb_we", 0)
    base = (record.get("retire_pc"), record.get("retire_instr"), wb_we)
    if wb_we != 1:
        return base
    return base + (
        record.get("wb_fp", 0), record.get("wb_rd"), record.get("wb_data"))


def differing_fields(
    rtl: dict[str, Any], gem5: dict[str, Any]
) -> list[str]:
    differences: list[str] = []

    def compare(keys: Iterable[str]) -> None:
        for key in keys:
            if rtl.get(key) != gem5.get(key):
                differences.append(key)

    compare(("reset", "cpu_cycle"))
    if rtl.get("reset") == 1 or gem5.get("reset") == 1:
        return differences

    compare(("ibus_req", "ibus_resp", "dbus_req", "dbus_resp",
             "grant", "retire", "stall_mask", "redirect", "done"))

    if rtl.get("ibus_req") == 1 or gem5.get("ibus_req") == 1:
        compare(("ibus_addr", "ibus_re"))
    if rtl.get("ibus_resp") == 1 or gem5.get("ibus_resp") == 1:
        compare(("ibus_r0", "ibus_r1", "ibus_r2", "ibus_r3"))
    if rtl.get("dbus_req") == 1 or gem5.get("dbus_req") == 1:
        compare(("dbus_addr", "dbus_re", "dbus_we", "dbus_wstrb",
                 "dbus_wdata"))
    if rtl.get("dbus_resp") == 1 or gem5.get("dbus_resp") == 1:
        compare(("dbus_rdata",))
    if rtl.get("retire") == 1 or gem5.get("retire") == 1:
        if normalized_retire(rtl) != normalized_retire(gem5):
            compare(("retire_pc", "retire_instr", "wb_we"))
            if rtl.get("wb_we") == 1 or gem5.get("wb_we") == 1:
                compare(("wb_fp", "wb_rd", "wb_data"))
    if rtl.get("redirect") == 1 or gem5.get("redirect") == 1:
        compare(("redirect_target",))
    if rtl.get("done") == 1 or gem5.get("done") == 1:
        compare(("done_value",))
    return differences


def first_cycle_mismatch(
    rtl: Trace, gem5: Trace
) -> tuple[int, list[str]] | None:
    rtl_edges = rtl.by_edge
    gem5_edges = gem5.by_edge
    for edge in sorted(set(rtl_edges) | set(gem5_edges)):
        if edge not in rtl_edges or edge not in gem5_edges:
            return edge, ["missing_edge"]
        differences = differing_fields(rtl_edges[edge], gem5_edges[edge])
        if differences:
            return edge, differences
    return None


def first_retire_mismatch(
    rtl: Trace, gem5: Trace
) -> tuple[int, dict[str, Any] | None, dict[str, Any] | None] | None:
    maximum = max(len(rtl.retires), len(gem5.retires))
    for index in range(maximum):
        rtl_event = rtl.retires[index] if index < len(rtl.retires) else None
        gem5_event = gem5.retires[index] if index < len(gem5.retires) else None
        if rtl_event is None or gem5_event is None:
            return index, rtl_event, gem5_event
        if (rtl_event["edge"] != gem5_event["edge"] or
                normalized_retire(rtl_event) != normalized_retire(gem5_event)):
            return index, rtl_event, gem5_event
    return None


def format_record(record: dict[str, Any] | None) -> str:
    if record is None:
        return "<missing>"
    preferred = (
        "edge", "cpu_cycle", "retire", "retire_pc", "retire_instr",
        "wb_we", "wb_fp", "wb_rd", "wb_data", "ibus_req", "ibus_addr",
        "ibus_resp", "dbus_req", "dbus_addr", "dbus_we", "dbus_wstrb",
        "dbus_resp", "stall_mask", "redirect", "redirect_target", "grant",
        "done", "done_value")
    return " ".join(f"{key}={record[key]}" for key in preferred
                    if key in record)


def window_lines(trace: Trace, center: int, radius: int) -> list[str]:
    by_edge = trace.by_edge
    lines: list[str] = []
    for edge in range(max(1, center - radius), center + radius + 1):
        if edge in by_edge:
            marker = ">" if edge == center else " "
            lines.append(f"{marker} {trace.source:5s} {format_record(by_edge[edge])}")
    return lines


def compare(rtl: Trace, gem5: Trace, window: int) -> dict[str, Any]:
    failures: list[str] = []
    rtl_done = rtl.done
    gem5_done = gem5.done
    done_match = (
        rtl_done is not None and gem5_done is not None and
        rtl_done["edge"] == gem5_done["edge"] and
        rtl_done.get("done_value") == gem5_done.get("done_value"))
    if not done_match:
        failures.append("DONE edge/value mismatch")

    retire_mismatch = first_retire_mismatch(rtl, gem5)
    if retire_mismatch is not None:
        failures.append("retire sequence/edge/writeback mismatch")

    cycle_mismatch = first_cycle_mismatch(rtl, gem5)
    if cycle_mismatch is not None:
        failures.append("cycle bus/control mismatch")

    centers: list[int] = []
    if cycle_mismatch is not None:
        centers.append(cycle_mismatch[0])
    if retire_mismatch is not None:
        _, rtl_event, gem5_event = retire_mismatch
        centers.extend(event["edge"] for event in (rtl_event, gem5_event)
                       if event is not None)
    if not done_match:
        centers.extend(event["edge"] for event in (rtl_done, gem5_done)
                       if event is not None)
    center = min(centers) if centers else None

    return {
        "match": not failures,
        "failures": failures,
        "rtl": {
            "path": str(rtl.path),
            "cycles": len(rtl.records),
            "retires": len(rtl.retires),
            "done_edge": None if rtl_done is None else rtl_done["edge"],
            "done_value": None if rtl_done is None else rtl_done.get("done_value"),
        },
        "gem5": {
            "path": str(gem5.path),
            "cycles": len(gem5.records),
            "retires": len(gem5.retires),
            "done_edge": None if gem5_done is None else gem5_done["edge"],
            "done_value": None if gem5_done is None else gem5_done.get("done_value"),
        },
        "retire_mismatch_index": (
            None if retire_mismatch is None else retire_mismatch[0]),
        "cycle_mismatch_edge": (
            None if cycle_mismatch is None else cycle_mismatch[0]),
        "cycle_mismatch_fields": (
            [] if cycle_mismatch is None else cycle_mismatch[1]),
        "window_center": center,
        "rtl_window": [] if center is None else window_lines(rtl, center, window),
        "gem5_window": [] if center is None else window_lines(gem5, center, window),
    }


def print_report(report: dict[str, Any]) -> None:
    status = "PASS" if report["match"] else "FAIL"
    print(f"BRS cycle comparison: {status}")
    for side in ("rtl", "gem5"):
        data = report[side]
        print(f"  {side:4s}: cycles={data['cycles']} retires={data['retires']} "
              f"done_edge={data['done_edge']} done_value={data['done_value']}")
    if report["match"]:
        print("  DONE, retire timing/writeback, and cycle bus/control all match.")
        return

    for failure in report["failures"]:
        print(f"  - {failure}")
    if report["retire_mismatch_index"] is not None:
        print(f"  first retire mismatch index: "
              f"{report['retire_mismatch_index']}")
    if report["cycle_mismatch_edge"] is not None:
        print(f"  first cycle mismatch edge: {report['cycle_mismatch_edge']} "
              f"fields={','.join(report['cycle_mismatch_fields'])}")
    if report["window_center"] is not None:
        print("\nRTL window:")
        print("\n".join(report["rtl_window"]))
        print("\ngem5 window:")
        print("\n".join(report["gem5_window"]))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compare Spirit RTL and gem5 canonical cycle traces")
    parser.add_argument("rtl_trace", type=Path)
    parser.add_argument("gem5_trace", type=Path)
    parser.add_argument("--window", type=int, default=8,
                        help="cycles before/after first mismatch to print")
    parser.add_argument("--json-report", type=Path,
                        help="also write the machine-readable result")
    args = parser.parse_args(argv)
    if args.window < 0:
        parser.error("--window must be non-negative")

    try:
        rtl = load_trace(args.rtl_trace)
        gem5 = load_trace(args.gem5_trace)
        report = compare(rtl, gem5, args.window)
    except (OSError, ValueError) as error:
        print(f"trace comparison error: {error}", file=sys.stderr)
        return 2

    print_report(report)
    if args.json_report:
        args.json_report.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
    return 0 if report["match"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
