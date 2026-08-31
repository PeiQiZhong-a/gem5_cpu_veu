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


TRACE_HEADERS = (
    "brs-cycle-trace-v1", "brs-cycle-trace-v2", "brs-cycle-trace-v3")
INT_RE = re.compile(r"^[+-]?[0-9]+$")
HEX_RE = re.compile(r"^0x[0-9a-f]+$")

V3_REQUIRED_HEADER_FIELDS = frozenset({
    "source", "sampling", "platform", "predictor_present", "btb_enabled",
})
V3_REQUIRED_RECORD_FIELDS = frozenset({
    "edge", "reset", "phase", "source", "platform", "cpu_cycle",
    "ibus_req", "ibus_addr", "ibus_re", "ibus_resp",
    "ibus_r0", "ibus_r1", "ibus_r2", "ibus_r3",
    "dbus_req", "dbus_addr", "dbus_re", "dbus_we", "dbus_wstrb",
    "dbus_wdata", "dbus_resp", "dbus_rdata",
    "retire", "retire_pc", "retire_instr", "wb_we", "wb_fp", "wb_rd",
    "wb_data", "stall_mask", "redirect", "redirect_target", "grant",
    "set_btb_off", "btb_match", "predict_failed",
    "hc_req", "hc_addr", "hc_re", "hc_we", "hc_write_type", "hc_wdata",
    "hc_vestart", "hc_valid", "hc_rdata",
    "done", "done_value", "error", "error_value",
})
V3_STRING_RECORD_FIELDS = frozenset({"phase", "source", "platform"})
V3_BOOLEAN_RECORD_FIELDS = frozenset({
    "reset", "ibus_req", "ibus_re", "ibus_resp", "dbus_req", "dbus_re",
    "dbus_we", "dbus_resp", "retire", "wb_we", "wb_fp", "redirect",
    "grant", "set_btb_off", "btb_match", "predict_failed", "hc_req",
    "hc_re", "hc_we", "hc_valid", "done", "error",
})


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
    version: int
    metadata: dict[str, Any]
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
    version = 0
    metadata: dict[str, Any] = {}
    saw_header = False
    previous_edge = 0

    with path.open("r", encoding="utf-8") as trace_file:
        for line_number, raw_line in enumerate(trace_file, 1):
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith("#"):
                matched_header = next(
                    (header for header in TRACE_HEADERS if header in line),
                    None)
                if matched_header is not None:
                    saw_header = True
                    version = int(matched_header.rsplit("v", 1)[1])
                    for token in line[1:].split():
                        if "=" in token:
                            key, value = token.split("=", 1)
                            metadata[key] = parse_value(value)
                    source = str(metadata.get("source", "unknown"))
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
            if version == 3 and edge != previous_edge + 1:
                raise ValueError(
                    f"{path}:{line_number}: v3 edge {edge} is not contiguous "
                    f"after {previous_edge}")
            if "reset" not in fields or "cpu_cycle" not in fields:
                raise ValueError(
                    f"{path}:{line_number}: missing reset/cpu_cycle")
            if version == 3:
                missing = sorted(V3_REQUIRED_RECORD_FIELDS - fields.keys())
                if missing:
                    raise ValueError(
                        f"{path}:{line_number}: brs-cycle-trace-v3 missing "
                        f"required fields: {','.join(missing)}")
                non_integer = sorted(
                    key for key in V3_REQUIRED_RECORD_FIELDS -
                    V3_STRING_RECORD_FIELDS
                    if not isinstance(fields[key], int))
                if non_integer:
                    raise ValueError(
                        f"{path}:{line_number}: v3 fields contain unknown or "
                        f"non-integer values: {','.join(non_integer)}")
                invalid_boolean = sorted(
                    key for key in V3_BOOLEAN_RECORD_FIELDS
                    if fields[key] not in (0, 1))
                if invalid_boolean:
                    raise ValueError(
                        f"{path}:{line_number}: v3 boolean fields are not "
                        f"0/1: {','.join(invalid_boolean)}")
                if (fields["ibus_re"] != fields["ibus_req"] or
                        fields["dbus_re"] + fields["dbus_we"] !=
                        fields["dbus_req"] or
                        fields["hc_req"] !=
                        int(bool(fields["hc_re"] or fields["hc_we"])) or
                        fields["grant"] != fields["hc_valid"]):
                    raise ValueError(
                        f"{path}:{line_number}: inconsistent v3 request/"
                        "valid aliases")
                if (metadata.get("btb_enabled") == 0 and
                        (fields["set_btb_off"] != 1 or
                         fields["btb_match"] != 0)):
                    raise ValueError(
                        f"{path}:{line_number}: no-BTB header requires "
                        "set_btb_off=1 and btb_match=0")
                if fields["source"] != source:
                    raise ValueError(
                        f"{path}:{line_number}: record source "
                        f"{fields['source']!r} differs from header {source!r}")
                if fields["platform"] != metadata.get("platform"):
                    raise ValueError(
                        f"{path}:{line_number}: record platform differs from "
                        "header")
                if fields["phase"] != metadata.get("sampling"):
                    raise ValueError(
                        f"{path}:{line_number}: record phase differs from "
                        "header sampling")
                if fields["reset"] != 0 or fields["cpu_cycle"] != edge:
                    raise ValueError(
                        f"{path}:{line_number}: v3 active interval requires "
                        "reset=0 and cpu_cycle=edge")
            previous_edge = edge
            records.append(fields)

    if not saw_header:
        raise ValueError(
            f"{path}: missing a supported trace header "
            f"({', '.join(TRACE_HEADERS)})")
    if not records:
        raise ValueError(f"{path}: no cycle records")
    if version == 3:
        missing = sorted(V3_REQUIRED_HEADER_FIELDS - metadata.keys())
        if missing:
            raise ValueError(
                f"{path}: brs-cycle-trace-v3 header missing required fields: "
                f"{','.join(missing)}")
        if metadata["sampling"] != "posedge-pre-nba":
            raise ValueError(
                f"{path}: unsupported v3 sampling {metadata['sampling']!r}")
        if metadata["btb_enabled"] not in (0, 1):
            raise ValueError(
                f"{path}: btb_enabled must be 0 or 1")
        if metadata["predictor_present"] not in (0, 1):
            raise ValueError(
                f"{path}: predictor_present must be 0 or 1")
    return Trace(path, source, version, metadata, tuple(records))


def select_comparison_window(
    trace: Trace,
    anchor_retire_pc: int | None = None,
    stop_at_done: bool = False,
) -> Trace:
    """Select and rebase the interval used for strict comparison.

    RTL regression traces include the boot ROM, while direct-application gem5
    traces begin at the application SRAM entry.  Anchoring both traces at the
    first retirement of the same application PC removes only that intentional
    startup-path difference.  All records from the anchor through DONE remain
    strict edge-by-edge comparisons.
    """
    start_edge = trace.records[0]["edge"]
    if anchor_retire_pc is not None:
        anchor = next(
            (
                record
                for record in trace.records
                if record.get("retire") == 1
                and record.get("retire_pc") == anchor_retire_pc
            ),
            None,
        )
        if anchor is None:
            raise ValueError(
                f"{trace.path}: no retired instruction at anchor PC "
                f"{anchor_retire_pc:#x}"
            )
        start_edge = anchor["edge"]

    end_edge = trace.records[-1]["edge"]
    if stop_at_done:
        done = next(
            (
                record
                for record in trace.records
                if record["edge"] >= start_edge and record.get("done") == 1
            ),
            None,
        )
        if done is None:
            raise ValueError(f"{trace.path}: no DONE record after window anchor")
        end_edge = done["edge"]

    selected: list[dict[str, Any]] = []
    for record in trace.records:
        if not start_edge <= record["edge"] <= end_edge:
            continue
        rebased = dict(record)
        rebased["original_edge"] = record["edge"]
        rebased["edge"] = record["edge"] - start_edge + 1
        rebased["cpu_cycle"] = rebased["edge"]
        selected.append(rebased)

    metadata = dict(trace.metadata)
    metadata["comparison_window_start_edge"] = start_edge
    metadata["comparison_window_end_edge"] = end_edge
    if anchor_retire_pc is not None:
        metadata["comparison_anchor_retire_pc"] = anchor_retire_pc
    return Trace(
        trace.path, trace.source, trace.version, metadata, tuple(selected)
    )


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
             "grant", "retire", "stall_mask", "redirect", "done",
             "error", "set_btb_off", "btb_match", "predict_failed",
             "hc_req", "hc_valid"))

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
    if rtl.get("hc_req") == 1 or gem5.get("hc_req") == 1:
        compare(("hc_addr", "hc_re", "hc_we", "hc_write_type",
                 "hc_wdata", "hc_vestart"))
    if rtl.get("hc_valid") == 1 or gem5.get("hc_valid") == 1:
        compare(("hc_rdata",))
    if rtl.get("done") == 1 or gem5.get("done") == 1:
        compare(("done_value",))
    if rtl.get("error") == 1 or gem5.get("error") == 1:
        compare(("error_value",))
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
        "set_btb_off", "btb_match", "predict_failed", "hc_req", "hc_addr",
        "hc_re", "hc_we", "hc_write_type", "hc_wdata", "hc_vestart",
        "hc_valid", "hc_rdata", "done", "done_value", "error",
        "error_value")
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
            "window_start_original_edge": rtl.metadata.get(
                "comparison_window_start_edge"
            ),
            "window_end_original_edge": rtl.metadata.get(
                "comparison_window_end_edge"
            ),
        },
        "gem5": {
            "path": str(gem5.path),
            "cycles": len(gem5.records),
            "retires": len(gem5.retires),
            "done_edge": None if gem5_done is None else gem5_done["edge"],
            "done_value": None if gem5_done is None else gem5_done.get("done_value"),
            "window_start_original_edge": gem5.metadata.get(
                "comparison_window_start_edge"
            ),
            "window_end_original_edge": gem5.metadata.get(
                "comparison_window_end_edge"
            ),
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
        if data["window_start_original_edge"] is not None:
            print(
                f"        original_window="
                f"{data['window_start_original_edge']}.."
                f"{data['window_end_original_edge']}"
            )
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
    parser.add_argument(
        "--anchor-retire-pc",
        type=lambda value: int(value, 0),
        help=(
            "start each trace at the first retirement of this PC and rebase "
            "that edge to one"
        ),
    )
    parser.add_argument(
        "--stop-at-done",
        action="store_true",
        help="discard records after the first DONE edge in the selected window",
    )
    args = parser.parse_args(argv)
    if args.window < 0:
        parser.error("--window must be non-negative")

    try:
        rtl = load_trace(args.rtl_trace)
        gem5 = load_trace(args.gem5_trace)
        rtl = select_comparison_window(
            rtl, args.anchor_retire_pc, args.stop_at_done
        )
        gem5 = select_comparison_window(
            gem5, args.anchor_retire_pc, args.stop_at_done
        )
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
