#!/usr/bin/env python3
"""Run Mikui-native firmware through gem5 and compare with RTL traces.

The RTL runner's result.txt is authoritative for testbench PASS/FAIL.  A
functional RTL failure is reported separately from a cycle-alignment failure.
"""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path

from compare_cycle_traces import (
    compare, first_veu_physical_mismatch, load_trace,
    select_comparison_window,
)


REPO = Path(__file__).resolve().parents[2]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-root", type=Path, required=True)
    parser.add_argument("--rtl-root", type=Path,
                        help="RTL run output containing per-case result.txt and trace")
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--gem5", type=Path,
                        default=REPO / "build/RISCV/gem5.opt")
    parser.add_argument("--profile", type=Path,
                        default=REPO / "configs/brs/veu_timing_profile.csv")
    parser.add_argument("--terminal-behavior", type=Path,
                        default=REPO / "configs/brs/veu_terminal_behavior.csv")
    parser.add_argument("--cases", help="Comma-separated firmware case names")
    parser.add_argument("--max-cycles", type=int, default=5000)
    parser.add_argument(
        "--enable-mikui-module", action="append", default=[],
        help="Enable a Mikui platform module in every gem5 run (repeatable)",
    )
    parser.add_argument(
        "--disable-mikui-module", action="append", default=[],
        help="Disable an optional Mikui platform module in every gem5 run",
    )
    parser.add_argument(
        "--check-veu-physical", action="store_true",
        help="Also compare raw 128-bit VEU SRAM pins (off for CPU-only runs)",
    )
    args = parser.parse_args()

    if args.max_cycles <= 0:
        parser.error("--max-cycles must be positive")
    input_root = args.input_root.resolve()
    rtl_root = args.rtl_root.resolve() if args.rtl_root else None
    output_root = args.output_root.resolve()
    gem5 = args.gem5.resolve()
    profile = args.profile.resolve()
    terminal_behavior = args.terminal_behavior.resolve()
    for label, path in (("input root", input_root), ("gem5", gem5),
                        ("timing profile", profile),
                        ("terminal behavior", terminal_behavior)):
        if not path.exists():
            parser.error(f"{label} does not exist: {path}")
    if rtl_root and not rtl_root.is_dir():
        parser.error(f"RTL result root does not exist: {rtl_root}")

    if args.cases:
        names = [name.strip() for name in args.cases.split(",") if name.strip()]
    else:
        names = sorted(path.name for path in input_root.iterdir()
                       if path.is_dir())
    if not names:
        parser.error("no cases selected")
    if len(names) != len(set(names)):
        parser.error("duplicate case names")

    output_root.mkdir(parents=True, exist_ok=True)
    summary = []
    for name in names:
        firmware = input_root / name
        instruction = firmware / "instruction.hex"
        memory = firmware / "memory.hex"
        if not instruction.is_file() or not memory.is_file():
            parser.error(f"{name}: instruction.hex or memory.hex missing")
        out = output_root / name
        out.mkdir(parents=True, exist_ok=True)
        trace_path = out / "brs_cycle_trace.log"
        # A failed rerun must never be classified using an older trace.
        trace_path.unlink(missing_ok=True)
        (out / "compare.json").unlink(missing_ok=True)
        command = [
            str(gem5), f"--outdir={out}",
            str(REPO / "configs/brs/run_pipeline_mini.py"),
            "--mem-system", "rtl-npu-lpnpu-mikui-decompress-dma",
            "--program-file", str(instruction),
            "--dmem-hex", str(memory),
            "--veu-model", "timing",
            "--veu-timing-profile", str(profile),
            "--veu-terminal-behavior", str(terminal_behavior),
            "--cycle-trace", str(trace_path),
            "--reset-cycles", "10",
            "--max-cycles", str(args.max_cycles),
            "--terminate-on-done-store",
            "--quiet-cycle-console",
        ]
        if args.check_veu_physical:
            command.extend(["--veu-cycle-trace",
                            str(out / "veu_cycle_trace.csv")])
        for module in args.enable_mikui_module:
            command.extend(["--enable-mikui-module", module])
        for module in args.disable_mikui_module:
            command.extend(["--disable-mikui-module", module])
        stimulus = "INDEPENDENT"
        with (out / "gem5.log").open("w", encoding="utf-8") as log:
            process = subprocess.run(command, cwd=REPO, stdout=log,
                                     stderr=subprocess.STDOUT, check=False)

        row = {
            "case": name, "rtl_status": "NOT_PROVIDED",
            "stimulus": stimulus,
            "gem5_status": "", "cpu_alignment": "NOT_COMPARED",
            "veu_physical": (
                "NOT_COMPARED" if args.check_veu_physical else "SKIPPED"),
            "rtl_done_edge": "", "gem5_done_edge": "",
            "mismatch_edge": "", "mismatch_fields": "",
            "veu_mismatch_edge": "", "veu_mismatch_fields": "",
        }
        gem5_trace = None
        if process.returncode != 0:
            row["gem5_status"] = f"EXIT_{process.returncode}"
        else:
            try:
                gem5_trace = load_trace(trace_path)
                row["gem5_done_edge"] = (
                    "" if gem5_trace.done is None else gem5_trace.done["edge"])
                row["gem5_status"] = (
                    "DONE" if gem5_trace.done else "NO_DONE")
            except (OSError, ValueError) as error:
                row["gem5_status"] = "INVALID_TRACE"
                (out / "trace_error.txt").write_text(
                    str(error) + "\n", encoding="utf-8")

        if rtl_root:
            rtl_out = rtl_root / name
            rtl_result = rtl_out / "result.txt"
            row["rtl_status"] = (rtl_result.read_text(
                encoding="utf-8").strip() if rtl_result.is_file()
                else "NO_RESULT")
            if row["rtl_status"] == "PASS" and gem5_trace is not None:
                try:
                    rtl_trace = load_trace(rtl_out / "brs_cycle_trace.log")
                    row["rtl_done_edge"] = (
                        "" if rtl_trace.done is None else rtl_trace.done["edge"])
                    rtl_window = select_comparison_window(
                        rtl_trace, stop_at_done=True)
                    gem5_window = select_comparison_window(
                        gem5_trace, stop_at_done=True)
                    report = compare(rtl_window, gem5_window, window=4)
                    row["cpu_alignment"] = (
                        "PASS" if report["match"] else "FAIL")
                    row["mismatch_edge"] = (
                        report["cycle_mismatch_edge"] or "")
                    row["mismatch_fields"] = ",".join(
                        report["cycle_mismatch_fields"])
                    if args.check_veu_physical:
                        veu_mismatch = first_veu_physical_mismatch(
                            rtl_window, gem5_window)
                        row["veu_physical"] = (
                            "PASS" if veu_mismatch is None else "FAIL")
                        if veu_mismatch is not None:
                            row["veu_mismatch_edge"] = veu_mismatch[0]
                            row["veu_mismatch_fields"] = ",".join(
                                veu_mismatch[1])
                    (out / "compare.json").write_text(
                        json.dumps(report, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
                except (OSError, ValueError) as error:
                    row["cpu_alignment"] = "COMPARE_ERROR"
                    (out / "compare_error.txt").write_text(
                        str(error) + "\n", encoding="utf-8")
            elif row["rtl_status"] != "PASS":
                row["cpu_alignment"] = "RTL_NOT_PASS"
            else:
                row["cpu_alignment"] = "GEM5_NOT_READY"

        summary.append(row)
        print(f"{name}: RTL={row['rtl_status']} gem5={row['gem5_status']} "
              f"stimulus={row['stimulus']} "
              f"cpu={row['cpu_alignment']} veu={row['veu_physical']} "
              f"cpu_edge={row['mismatch_edge']} "
              f"veu_edge={row['veu_mismatch_edge']}", flush=True)

    with (output_root / "summary.csv").open(
            "w", newline="", encoding="utf-8") as out_file:
        writer = csv.DictWriter(out_file, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)
    comparable = ([row for row in summary if row["rtl_status"] == "PASS"]
                  if rtl_root else summary)
    cpu_pass = sum(row["cpu_alignment"] == "PASS" for row in comparable)
    print(f"TOTAL={len(summary)} CPU_COMPARABLE={len(comparable)} "
          f"CPU_PASS={cpu_pass} RTL_NOT_PASS={len(summary) - len(comparable)}",
          flush=True)
    return 0 if comparable and all(
        row["cpu_alignment"] in ("PASS", "NOT_COMPARED") and
        (not args.check_veu_physical or row["veu_physical"] == "PASS") and
        row["gem5_status"] == "DONE" for row in comparable) else 1


if __name__ == "__main__":
    sys.exit(main())
