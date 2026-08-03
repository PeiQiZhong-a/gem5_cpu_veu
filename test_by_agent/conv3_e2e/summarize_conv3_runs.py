#!/usr/bin/env python3
"""Summarize the supported stub/sau_n gem5 reports and calculate signed deltas."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


RUNS = {
    "A_archive_baseline_stub": "archive_baseline-stub",
    "B_generated_legacy_stub": "generated_legacy_control-stub",
    "C_stub": "generated_four_ins_control_matched-stub",
    "C_sau_n": "generated_four_ins_control_matched-sau_n",
    "D_full_offload_sau_n": "generated_four_ins_full_offload-sau_n",
}


def read_trace_regions(
    trace_path: Path, manifest: dict[str, Any]
) -> dict[str, int | None]:
    layout = manifest.get("program_layout")
    if not layout:
        return {
            "startup_retired": None,
            "startup_cycles": None,
            "input_init_retired": None,
            "input_init_cycles": None,
            "padding_retired": None,
            "padding_cycles": None,
            "config_retired": None,
            "config_cycles": None,
            "tail_retired": None,
            "tail_cycles": None,
        }

    regions = {
        "startup_retired": tuple(layout.get("startup_region", ())),
        "input_init_retired": tuple(layout.get("input_init_region", ())),
        "padding_retired": tuple(layout.get("padding_region", ())),
        "config_retired": tuple(layout.get("config_region", ())),
        "tail_retired": (int(layout.get("shared_tail_pc", 0)), 0x100000),
    }
    counts = {name: 0 for name in regions}
    cycles = {name: 0 for name in regions}
    for raw_line in trace_path.read_text(encoding="utf-8").splitlines():
        if not raw_line or raw_line.startswith("#"):
            continue
        fields: dict[str, int] = {}
        for token in raw_line.split():
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            try:
                fields[key] = int(value, 0)
            except ValueError:
                continue
        if fields.get("reset") != 0:
            continue
        cycle_pc = fields.get("cpu_pc")
        if cycle_pc is not None:
            for name, (start, end) in regions.items():
                if start <= cycle_pc < end:
                    cycles[name] += 1
        if fields.get("retire") != 1:
            continue
        pc = fields.get("retire_pc")
        if pc is None:
            continue
        for name, (start, end) in regions.items():
            if start <= pc < end:
                counts[name] += 1
    counts.update({
        name.replace("_retired", "_cycles"): value
        for name, value in cycles.items()
    })
    return counts


def load_case(
    output_root: Path,
    fixture_root: Path,
    case_name: str,
    directory: str,
) -> dict[str, Any]:
    run_dir = output_root / directory
    report = json.loads((run_dir / "report.json").read_text(encoding="utf-8"))
    fixture_dir = (
        output_root / "archive_baseline_fixture"
        if case_name == "A_archive_baseline_stub"
        else fixture_root / report["fixture"]
    )
    manifest = json.loads(
        (fixture_dir / "manifest.json").read_text(encoding="utf-8")
    )
    details = report["details"]
    instruction_count = sum(details["sau_instruction_counts"].values())
    roi_start = details.get("roi_start_cycle")
    roi_end = details.get("roi_end_cycle")
    roi_cycles = None if roi_start is None or roi_end is None else roi_end - roi_start
    roi_retired = report["roi_end_retired_inst"] - report["roi_start_retired_inst"]
    return {
        "run_directory": str(run_dir),
        "fixture": report["fixture"],
        "sau_model": report["sau_model"],
        "cycle_count": report["cycle_count"],
        "retired_inst_count": report["retired_inst_count"],
        "stall_count": report["stall_count"],
        "sau_config_instruction_count": instruction_count,
        "sau_handshake_stall_cycles": report["sau_csr_handshake_cycles"],
        "sau_compute_wait_cycles": details.get("compute_wait_cycles", 0),
        "sau_writeback_wait_cycles": details.get("writeback_wait_cycles", 0),
        "sau_memory_requests": details.get("memory_requests", 0),
        "roi_cycles": roi_cycles,
        "roi_retired_instructions": roi_retired,
        "non_sau_retired_instructions": (
            report["retired_inst_count"] - instruction_count
        ),
        **read_trace_regions(run_dir / "cycle_trace.log", manifest),
    }


def percent_delta(before: int, delta: int) -> float | None:
    if before == 0:
        return None
    return 100.0 * delta / before


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--fixture-root", type=Path, required=True)
    parser.add_argument("--json-report", type=Path, required=True)
    args = parser.parse_args()

    output_root = args.output_root.resolve()
    fixture_root = args.fixture_root.resolve()
    cases = {
        name: load_case(output_root, fixture_root, name, directory)
        for name, directory in RUNS.items()
    }
    comparisons: dict[str, Any] = {}
    pairs = {
        "A_vs_B": ("A_archive_baseline_stub", "B_generated_legacy_stub"),
        "B_vs_C_stub": ("B_generated_legacy_stub", "C_stub"),
        "C_stub_vs_C_sau_n": ("C_stub", "C_sau_n"),
        "C_sau_n_vs_D_sau_n": ("C_sau_n", "D_full_offload_sau_n"),
        "A_vs_D_sau_n": (
            "A_archive_baseline_stub", "D_full_offload_sau_n"
        ),
    }
    metrics = (
        "cycle_count",
        "retired_inst_count",
        "stall_count",
        "sau_config_instruction_count",
        "sau_handshake_stall_cycles",
        "sau_compute_wait_cycles",
        "sau_writeback_wait_cycles",
        "roi_cycles",
        "roi_retired_instructions",
        "non_sau_retired_instructions",
        "sau_memory_requests",
        "startup_cycles",
        "input_init_cycles",
        "padding_cycles",
        "config_cycles",
        "tail_cycles",
    )
    for name, (before_name, after_name) in pairs.items():
        before = cases[before_name]
        after = cases[after_name]
        delta: dict[str, Any] = {}
        for metric in metrics:
            before_value = before[metric]
            after_value = after[metric]
            if before_value is None or after_value is None:
                delta[metric] = {"before": before_value, "after": after_value}
                continue
            change = after_value - before_value
            delta[metric] = {
                "before": before_value,
                "after": after_value,
                "absolute": change,
                "percent": percent_delta(before_value, change),
            }
        comparisons[name] = {
            "before": before_name,
            "after": after_name,
            "metrics": delta,
        }

    report = {
        "status": "PASS",
        "runs": cases,
        "comparisons": comparisons,
        "interpretation": {
            "B_vs_C_stub": "controlled four-instruction ABI CPU overhead",
            "C_stub_vs_C_sau_n": "stub response versus sau_n local-scratchpad wait",
            "C_sau_n_vs_D_sau_n": "software-padding/preprocessing boundary",
            "A_vs_D_sau_n": "overall artifact difference with compiler/allocation/path confounders",
        },
    }
    args.json_report.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
