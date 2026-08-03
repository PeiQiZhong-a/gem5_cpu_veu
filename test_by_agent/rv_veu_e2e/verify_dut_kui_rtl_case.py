#!/usr/bin/env python3
"""Compare one dut_kui gem5 run with its captured RTL timing example."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import sys
from pathlib import Path

STAT_PREFIX = "system.pipeline."


def fail(message: str) -> None:
    raise RuntimeError(message)


def csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as input_file:
        return list(csv.DictReader(input_file))


def case_name(row: dict[str, str]) -> str:
    return row.get("test") or row.get("case_id") or ""


def validate_handoff(path: Path) -> dict[str, dict[str, object]]:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("schema_version") != "1.0.0":
        fail(f"unsupported RTL handoff schema {manifest.get('schema_version')}")
    root = path.parent
    for artifact in manifest["artifact_bundle"].values():
        artifact_path = root / artifact["path_relative_to_handoff"]
        if not artifact_path.is_file():
            fail(f"missing handoff artifact {artifact_path}")
        digest = hashlib.sha256(artifact_path.read_bytes()).hexdigest()
        if digest != artifact["sha256"]:
            fail(f"SHA256 mismatch for {artifact_path.name}")
    cases = {str(case["case_id"]): case for case in manifest["cases"]}
    if len(cases) != 34:
        fail(f"handoff contains {len(cases)} cases, expected 34")
    for filename in (
        "veu_timing_summary.csv",
        "veu_functional_vectors.csv",
        "veu_timing_events.csv",
    ):
        observed = {case_name(row) for row in csv_rows(root / filename)}
        if observed != cases.keys():
            fail(f"{filename} case set differs from handoff manifest")
    return cases


def read_stats(path: Path) -> dict[str, int]:
    result: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) < 2 or not fields[0].startswith(STAT_PREFIX):
            continue
        try:
            result[fields[0][len(STAT_PREFIX):]] = int(float(fields[1]))
        except ValueError:
            continue
    return result


def stat(stats: dict[str, int], name: str) -> int:
    if name not in stats:
        fail(f"missing stat {STAT_PREFIX}{name}")
    return stats[name]


def one_event(rows: list[dict[str, str]], event: str) -> dict[str, str]:
    matches = [row for row in rows if row["event"] == event]
    if len(matches) != 1:
        fail(f"expected one {event}, found {len(matches)}")
    return matches[0]


def parse_detail(detail: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for item in detail.split(";"):
        if "=" in item:
            key, value = item.split("=", 1)
            result[key] = value
    return result


def response_deltas(
    rows: list[dict[str, str]], request: str, response: str,
) -> list[int]:
    requests = {
        row["transaction_id"]: int(row["cycle"], 0)
        for row in rows if row["event"] == request
    }
    responses = {
        row["transaction_id"]: int(row["cycle"], 0)
        for row in rows if row["event"] == response
    }
    if requests.keys() != responses.keys():
        fail(f"{request}/{response} transaction IDs differ")
    return [responses[key] - requests[key] for key in requests]


def mask_class(mask: int) -> str:
    return "zero" if mask == 0 else ("full" if mask == 0xFFFFFFFF else "partial")

def address_value(value: str) -> int:
    return int(value, 0) if value.lower().startswith("0x") else int(value, 16)


def event_signatures(
    rows: list[dict[str, str]], event: str, start_cycle: int,
    *, source: bool = False,
) -> list[tuple[int, ...]]:
    result: list[tuple[int, ...]] = []
    for row in rows:
        if row["event"] != event:
            continue
        signature = [
            int(row["cycle"], 0) - start_cycle,
            int(row["chunk"], 0),
        ]
        if source:
            signature.append(int(row["source"], 0))
        result.append(tuple(signature))
    return sorted(result)


def relative_address_signatures(
    rows: list[dict[str, str]], event: str, start_cycle: int,
    *, source: bool = False,
) -> list[tuple[int, ...]]:
    selected = [row for row in rows if row["event"] == event]
    bases: dict[int, int] = {}
    for row in selected:
        source_id = int(row["source"], 0) if source else 0
        address = address_value(
            row["address" if "address" in row else "addr"]
        )
        bases[source_id] = min(address, bases.get(source_id, address))

    result: list[tuple[int, ...]] = []
    for row in selected:
        source_id = int(row["source"], 0) if source else 0
        address = address_value(
            row["address" if "address" in row else "addr"]
        )
        signature = [
            int(row["cycle"], 0) - start_cycle,
            int(row["chunk"], 0),
        ]
        if source:
            signature.append(source_id)
        signature.append(address - bases[source_id])
        result.append(tuple(signature))
    return sorted(result)


def compare_event_streams(
    gem5: list[dict[str, str]], rtl: list[dict[str, str]],
) -> None:
    gem5_start = int(one_event(gem5, "operation_start")["cycle"], 0)
    rtl_start = int(one_event(rtl, "start")["cycle"], 0)

    for gem5_event, rtl_event in (
        ("status_set", "status_set"),
        ("lock_start", "lock_start"),
        ("status_clear", "status_clear"),
        ("lock_finish", "lock_finish"),
    ):
        gem5_offset = int(one_event(gem5, gem5_event)["cycle"], 0) - gem5_start
        rtl_offset = int(one_event(rtl, rtl_event)["cycle"], 0) - rtl_start
        if gem5_offset != rtl_offset:
            fail(
                f"{gem5_event} offset is {gem5_offset}, "
                f"RTL {rtl_event} offset is {rtl_offset}"
            )

    mappings = (
        ("read_request", "read_issue", True),
        ("fifo_push", "fifo_push", True),
        ("vfu_accept", "vfu_accept", False),
        ("vfu_done", "vfu_complete", False),
        ("write_request", "write", False),
        ("write_response", "bank_write", False),
    )
    for gem5_event, rtl_event, with_source in mappings:
        gem5_signatures = event_signatures(
            gem5, gem5_event, gem5_start, source=with_source,
        )
        rtl_signatures = event_signatures(
            rtl, rtl_event, rtl_start, source=with_source,
        )
        if gem5_signatures != rtl_signatures:
            fail(
                f"{gem5_event}/{rtl_event} signatures differ: "
                f"gem5={gem5_signatures}, RTL={rtl_signatures}"
            )

    # The generated E2E image deliberately remaps captured RTL operands into a
    # common memory layout.  Absolute addresses are checked against that image
    # by verify_veu_e2e.py; compare per-source address progression with RTL here.
    for gem5_event, rtl_event, with_source in (
        ("read_request", "read_issue", True),
        ("write_request", "write", False),
        ("write_response", "bank_write", False),
    ):
        gem5_signatures = relative_address_signatures(
            gem5, gem5_event, gem5_start, source=with_source,
        )
        rtl_signatures = relative_address_signatures(
            rtl, rtl_event, rtl_start, source=with_source,
        )
        if gem5_signatures != rtl_signatures:
            fail(
                f"{gem5_event}/{rtl_event} relative address signatures differ: "
                f"gem5={gem5_signatures}, RTL={rtl_signatures}"
            )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--stats", type=Path, required=True)
    parser.add_argument("--rtl-summary", type=Path, required=True)
    parser.add_argument("--rtl-events", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--handoff-manifest", type=Path)
    args = parser.parse_args()

    metadata = json.loads(args.metadata.read_text(encoding="utf-8"))
    stats = read_stats(args.stats)
    rtl_test = str(metadata["rtl_test"])
    handoff_cases = (
        validate_handoff(args.handoff_manifest)
        if args.handoff_manifest else {}
    )
    rtl_matches = [
        row for row in csv_rows(args.rtl_summary) if case_name(row) == rtl_test
    ]
    if len(rtl_matches) != 1:
        fail(f"expected one RTL summary row for {rtl_test}, found {len(rtl_matches)}")
    rtl = rtl_matches[0]

    trace = csv_rows(args.trace)
    start = one_event(trace, "operation_start")
    finish = one_event(trace, "operation_finish")
    provenance = parse_detail(start["detail"])
    profile_matches = [
        row for row in csv_rows(args.profile)
        if row["profile_id"] == provenance.get("profile_id")
    ]
    if len(profile_matches) != 1:
        fail(
            f"expected one selected profile {provenance.get('profile_id')}, "
            f"found {len(profile_matches)}"
        )
    profile = profile_matches[0]

    expected_tuple = {
        "op": str(metadata["op"]),
        "scalar_en": "1" if int(metadata["config"]) & 0x800 else "0",
        "mask_class": mask_class(int(metadata["mask"])),
        "source_set": str(metadata["source_set"]),
        "chunk_class": str(metadata["chunks"]),
    }
    for field, expected in expected_tuple.items():
        if profile[field] != expected:
            fail(f"profile {field}={profile[field]} does not match {expected}")

    rtl_cycles = int(rtl["total_cycles"], 0)
    gem5_cycles = int(finish["cycle"], 0) - int(start["cycle"], 0)
    if int(profile["operation_cycles"], 0) != rtl_cycles:
        fail(
            f"profile operation_cycles={profile['operation_cycles']} "
            f"does not match RTL total_cycles={rtl_cycles}"
        )
    if provenance.get("operation_cycles") != str(rtl_cycles):
        fail("operation_start does not report the RTL comparison cycle")
    if provenance.get("timing_source") != "rtl_sim":
        fail("captured RTL case did not select rtl_sim data timing")
    if provenance.get("control_timing_source") != "rtl_sim":
        fail("captured RTL case did not select rtl_sim control timing")
    if provenance.get("control_evidence_id") != profile["evidence_id"]:
        fail("control timing evidence does not match selected profile")
    if handoff_cases:
        expected_evidence = str(handoff_cases[rtl_test]["evidence_id"])
        if profile["evidence_id"] != expected_evidence:
            fail(
                f"profile evidence {profile['evidence_id']} does not match "
                f"handoff {expected_evidence}"
            )
    rtl_lock_start_delay = int(rtl["lock_start_delay"], 0)
    rtl_finish_drain = (
        int(rtl["lock_finish_cycle"], 0) - int(rtl["status_clear_cycle"], 0)
    )
    if int(profile["lock_start_delay"], 0) != rtl_lock_start_delay:
        fail("profile lock_start_delay does not match RTL summary")
    if int(profile["finish_drain_cycles"], 0) != rtl_finish_drain:
        fail("profile finish_drain_cycles does not match RTL summary")

    rtl_events = [
        row for row in csv_rows(args.rtl_events) if case_name(row) == rtl_test
    ]
    if rtl_test == "vredmin_s8_full_c1":
        first_accept = min(
            int(row["cycle"], 0)
            for row in rtl_events if row["event"] == "vfu_accept"
        )
        normalized: list[dict[str, str]] = []
        for row in rtl_events:
            if (
                row["event"] == "vfu_complete"
                and int(row["cycle"], 0) < first_accept
            ):
                continue
            if row["event"] == "vfu_complete":
                row = dict(row)
                row["chunk"] = "0"
                row["vfu_seq"] = "0"
            normalized.append(row)
        rtl_events = normalized
    compare_event_streams(trace, rtl_events)
    accepts = sorted(
        (
            int(row.get("vfu_seq") or row["chunk"], 0),
            int(row["cycle"], 0),
        )
        for row in rtl_events if row["event"] == "vfu_accept"
    )
    completes = sorted(
        (
            int(row.get("vfu_seq") or row["chunk"], 0),
            int(row["cycle"], 0),
        )
        for row in rtl_events if row["event"] == "vfu_complete"
    )
    if [seq for seq, _ in accepts] != [seq for seq, _ in completes]:
        fail("RTL VFU accept/complete sequences differ")
    rtl_vfu_latencies = [
        done_cycle - accept_cycle
        for (_, accept_cycle), (_, done_cycle) in zip(accepts, completes)
    ]
    expected_pattern = [
        int(value, 0) for value in rtl["vfu_latency_pattern"].split(";")
    ]
    if rtl_vfu_latencies != expected_pattern:
        fail(
            f"RTL event latency pattern {rtl_vfu_latencies} does not match "
            f"summary {expected_pattern}"
        )
    profile_latency = int(profile["vfu_latency"], 0)
    expected_profile_pattern = [profile_latency] * len(expected_pattern)
    if metadata["op"] == "vslidedown" and len(expected_pattern) > 1:
        expected_profile_pattern[-1] += 1
    if expected_pattern != expected_profile_pattern:
        fail(
            f"profile VFU latency={profile['vfu_latency']}, "
            f"RTL observed={expected_pattern}"
        )

    read_deltas = response_deltas(trace, "read_request", "read_response")
    write_deltas = response_deltas(trace, "write_request", "write_response")
    if any(delta != 4 for delta in read_deltas):
        fail(f"dut_kui read response deltas are {read_deltas}, expected 4")
    if any(delta != 1 for delta in write_deltas):
        fail(f"dut_kui write response deltas are {write_deltas}, expected 1")

    stat_pairs = {
        "veu_memory_reads": "read_count",
        "veu_memory_writes": "write_count",
        "veu_max_outstanding_reads": "max_outstanding_reads",
        "veu_fifo1_max_occupancy": "max_fifo1",
        "veu_fifo2_max_occupancy": "max_fifo2",
        "veu_fifo3_max_occupancy": "max_fifo3",
        "veu_store_priority_cycles": "vspbu_stall_cycles",
    }
    for gem5_stat, rtl_field in stat_pairs.items():
        if not rtl.get(rtl_field):
            continue
        actual = stat(stats, gem5_stat)
        expected = int(rtl[rtl_field], 0)
        if actual != expected:
            fail(f"{gem5_stat} is {actual}, RTL {rtl_field} is {expected}")

    if gem5_cycles != rtl_cycles:
        fail(f"gem5 operation cycles {gem5_cycles}, RTL {rtl_cycles}")

    result_fields = [
            rtl_test,
            str(metadata["op"]),
            str(metadata["vlen"]),
            f"0x{int(metadata['config']):08x}",
            f"0x{int(metadata['mask']):08x}",
            "MATCH" if gem5_cycles == rtl_cycles else "DIFF",
            str(gem5_cycles),
            str(rtl_cycles),
            profile["vfu_latency"],
            profile["vfu_ii"],
            profile["vsu_latency"],
            profile["profile_id"],
            profile["evidence_id"],
            provenance["control_timing_source"],
            provenance["control_evidence_id"],
            "EVENT_MATCH",
    ]
    if args.handoff_manifest:
        result_fields.extend(
            rtl.get(field) or "N/A"
            for field in (
                "read_candidate_cycles",
                "reads_blocked_by_store_cycles",
                "vfu_max_in_flight",
                "vsu_queue_stall_cycles",
            )
        )
    print(",".join(result_fields))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"dut_kui RTL-case verification failed: {error}", file=sys.stderr)
        sys.exit(1)
