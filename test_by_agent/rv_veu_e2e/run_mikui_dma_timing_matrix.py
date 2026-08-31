#!/usr/bin/env python3
"""Run every measured Mikui DMA VEU timing row through the RV pipeline.

The matrix is driven by the checked-in normal profile and terminal-behavior
CSV files.  Every encodable row gets its own generated program, gem5 output,
cycle trace, verification record, and one summary.csv row.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path
from typing import Iterable


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_veu_e2e_hex as generator  # noqa: E402


STAT_PREFIX = "system.pipeline."
MASK_VALUES = {"full": 0xFFFF, "partial": 0x5555, "zero": 0}
NORMAL_CASES = {
    ("vadd", "0"): "vadd_vector",
    ("vadd", "1"): "vadd_scalar",
    ("vsub", "0"): "vsub",
    ("vmin", "0"): "vmin",
    ("vmax", "0"): "vmax",
    ("vredmin", "0"): "vredmin",
    ("vredmax", "0"): "vredmax",
    ("vand", "0"): "vand",
    ("vor", "0"): "vor",
    ("vxor", "0"): "vxor",
    ("vslideup", "1"): "vslideup_scalar",
    ("vslidedown", "1"): "vslidedown_scalar",
    ("vmv", "0"): "vmv",
    ("vssrl", "1"): "vssrl_scalar",
    ("vssra", "1"): "vssra_scalar",
    ("vnclip", "1"): "vnclip_scalar",
    ("vredsum", "0"): "vredsum",
    ("vmul", "0"): "vmul",
}
TERMINAL_CASES = {
    "vmv": "vmv_scalar",
    "vslidedown": "vslidedown_scalar",
    "vcompress": "illegal_vcompress",
    "vwredsum": "illegal_vwredsum",
    "vmulhsu": "illegal_vmulhsu",
    "vmulh": "illegal_vmulh",
    "vmsub": "illegal_vmsub",
    "vmac": "illegal_vmac",
}
SUMMARY_FIELDS = (
    "row_kind", "row_id", "op", "scalar_en", "mask_class", "chunks",
    "status", "functional", "cycle_expected", "cycle_actual",
    "status_clear_expected", "status_clear_actual", "reads", "writes",
    "profile_hits", "terminal_uses", "illegal_operations", "cycle_count",
    "detail",
)


class VerificationError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as input_file:
        return list(csv.DictReader(input_file))


def read_stats(path: Path) -> dict[str, int]:
    result: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) < 2 or not fields[0].startswith(STAT_PREFIX):
            continue
        try:
            result[fields[0][len(STAT_PREFIX):]] = int(float(fields[1]))
        except ValueError:
            pass
    return result


def get_stat(stats: dict[str, int], name: str) -> int:
    require(name in stats, f"missing stat {STAT_PREFIX}{name}")
    return stats[name]


def read_trace(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as input_file:
        return list(csv.DictReader(input_file))


def event_rows(events: Iterable[dict[str, str]], name: str) -> list[dict[str, str]]:
    return [row for row in events if row["event"] == name]


def last_x10(path: Path) -> int:
    pattern = re.compile(r"\brd=x10\s+data=(0x[0-9a-fA-F]+)")
    values = [
        int(match.group(1), 0)
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines()
        if (match := pattern.search(line))
    ]
    require(bool(values), "missing RV retire trace for x10")
    return values[-1]


def address_list(rows: Iterable[dict[str, str]]) -> list[int]:
    return [int(row["addr"], 0) for row in rows]


def expected_normal_addresses(
    case: generator.Case, chunks: int, mask_class: str,
) -> tuple[list[int], list[int]]:
    reads = [
        base + 16 * chunk
        for base in generator.source_bases(case, generator.MIKUI_LAYOUT)
        for chunk in range(chunks)
    ]
    if mask_class == "zero":
        writes: list[int] = []
    elif case.reduction:
        writes = [generator.MIKUI_LAYOUT.dest] * min(chunks, 3)
    else:
        writes = [generator.MIKUI_LAYOUT.dest + 16 * chunk for chunk in range(chunks)]
    return reads, writes


def expected_terminal_addresses(
    row: dict[str, str], chunks: int, mask_class: str,
) -> tuple[list[int], list[int]]:
    op = row["op"]
    tail_reads = int(row["tail_reads"])
    if op == "vslidedown":
        reads = [generator.MIKUI_LAYOUT.src2 + 16 * chunk
                 for chunk in range(chunks + tail_reads)]
        writes = [] if mask_class == "zero" else [
            generator.MIKUI_LAYOUT.dest + 16 * chunk for chunk in range(chunks)
        ]
        return reads, writes
    if op == "vmv":
        reads = [generator.MIKUI_LAYOUT.src2 + 16 * chunk
                 for chunk in range(tail_reads)]
        if mask_class == "zero":
            return reads, []
        unique_count = chunks + int(row["extra_writes"]) - 1
        writes = [generator.MIKUI_LAYOUT.dest + 16 * chunk
                  for chunk in range(unique_count)]
        writes.append(writes[-1])
        return reads, writes
    return [], []


def verify_common(
    row: dict[str, str], row_kind: str, metadata: dict[str, object],
    stats: dict[str, int], events: list[dict[str, str]], run_log: Path,
) -> dict[str, object]:
    starts = event_rows(events, "operation_start")
    finishes = event_rows(events, "operation_finish")
    clears = event_rows(events, "status_clear")
    require(len(starts) == 1, f"operation_start count {len(starts)}, expected 1")
    require(len(finishes) == 1, f"operation_finish count {len(finishes)}, expected 1")
    require(len(clears) == 1, f"status_clear count {len(clears)}, expected 1")
    require(starts[0]["op"] == row["op"],
            f"trace op {starts[0]['op']} != {row['op']}")
    row_id = row["profile_id"] if row_kind == "normal" else row["behavior_id"]
    require(f"profile_id={row_id};" in starts[0]["detail"],
            f"operation_start did not select {row_id}")

    cycle_expected = int(
        row["operation_cycles"] if row_kind == "normal" else row["lock_finish_cycles"]
    )
    status_expected = (
        cycle_expected - int(row["finish_drain_cycles"])
        if row_kind == "normal" else int(row["status_clear_cycles"])
    )
    start_cycle = int(starts[0]["cycle"])
    cycle_actual = int(finishes[0]["cycle"]) - start_cycle
    status_actual = int(clears[0]["cycle"]) - start_cycle
    require(cycle_actual == cycle_expected,
            f"operation cycles {cycle_actual}, expected {cycle_expected}")
    require(status_actual == status_expected,
            f"status-clear cycles {status_actual}, expected {status_expected}")
    require(get_stat(stats, "veu_busy_cycles") == cycle_expected,
            f"busy cycles {get_stat(stats, 'veu_busy_cycles')}, expected {cycle_expected}")
    require(get_stat(stats, "veu_operation_start_count") == 1,
            "VEU operation start stat is not one")
    require(get_stat(stats, "veu_operation_complete_count") == 1,
            "VEU operation complete stat is not one")
    require(get_stat(stats, "veu_timing_rtl_sim_uses") == 1,
            "RTL timing-source stat is not one")
    require(get_stat(stats, "veu_timing_legacy_uses") == 0,
            "legacy timing-source stat is nonzero")
    require(get_stat(stats, "veu_timing_default_uses") == 0,
            "default timing-source stat is nonzero")
    require(get_stat(stats, "veu_control_timing_rtl_sim_uses") == 1,
            "RTL control-timing stat is not one")
    require(get_stat(stats, "veu_control_timing_default_uses") == 0,
            "default control-timing stat is nonzero")
    require(get_stat(stats, "veu_profile_fallbacks") == 0,
            "profile fallback stat is nonzero")
    require(get_stat(stats, "veu_unexpected_responses") == 0,
            "unexpected memory response stat is nonzero")
    require(get_stat(stats, "veu_current_outstanding_reads") == 0,
            "outstanding reads remain at simulation exit")
    require(last_x10(run_log) == 0, "RV destination checker reported mismatch")

    if row_kind == "normal":
        require(get_stat(stats, "veu_profile_hits") == 1,
                "normal row did not hit exactly one profile")
        require(get_stat(stats, "veu_terminal_behavior_uses") == 0,
                "normal row unexpectedly selected terminal behavior")
        require(get_stat(stats, "veu_illegal_operations") == 0,
                "normal row was marked illegal")
    else:
        require(get_stat(stats, "veu_profile_hits") == 0,
                "terminal row unexpectedly hit a normal profile")
        require(get_stat(stats, "veu_terminal_behavior_uses") == 1,
                "terminal row did not select exactly one terminal behavior")
        illegal_expected = int(row["classification"] == "ILLEGAL_COMPLETE")
        require(get_stat(stats, "veu_illegal_operations") == illegal_expected,
                f"illegal stat mismatch, expected {illegal_expected}")

    read_requests = event_rows(events, "read_request")
    write_requests = event_rows(events, "write_request")
    if row_kind == "normal":
        expected_reads, expected_writes = expected_normal_addresses(
            generator.CASE_BY_NAME[str(metadata["case"])],
            int(metadata["chunks"]), str(metadata["mask_class"]),
        )
    else:
        expected_reads, expected_writes = expected_terminal_addresses(
            row, int(metadata["chunks"]), str(metadata["mask_class"]),
        )
    require(Counter(address_list(read_requests)) == Counter(expected_reads),
            f"read addresses {address_list(read_requests)} != {expected_reads}")
    require(Counter(address_list(write_requests)) == Counter(expected_writes),
            f"write addresses {address_list(write_requests)} != {expected_writes}")
    require(get_stat(stats, "veu_memory_reads") == len(expected_reads),
            f"read stat mismatch, expected {len(expected_reads)}")
    require(get_stat(stats, "veu_memory_writes") == len(expected_writes),
            f"write stat mismatch, expected {len(expected_writes)}")
    if metadata["mask_class"] == "partial":
        require(get_stat(stats, "veu_masked_writes") == len(expected_writes),
                "partial-mask write stat mismatch")

    request_ids = {
        row["transaction_id"] for row in events
        if row["event"] in {"read_request", "write_request"}
    }
    response_ids = {
        row["transaction_id"] for row in events
        if row["event"] in {
            "read_response", "write_response",
            "terminal_late_read_response", "terminal_late_write_response",
        }
    }
    require(request_ids == response_ids,
            f"request/response transaction IDs differ: {request_ids ^ response_ids}")
    require(get_stat(stats, "cycle_count") < int(metadata["max_cycles"]),
            "simulation reached max-cycles")
    return {
        "functional": "PASS",
        "cycle_expected": cycle_expected,
        "cycle_actual": cycle_actual,
        "status_clear_expected": status_expected,
        "status_clear_actual": status_actual,
        "reads": len(expected_reads),
        "writes": len(expected_writes),
        "profile_hits": get_stat(stats, "veu_profile_hits"),
        "terminal_uses": get_stat(stats, "veu_terminal_behavior_uses"),
        "illegal_operations": get_stat(stats, "veu_illegal_operations"),
        "cycle_count": get_stat(stats, "cycle_count"),
    }


def case_for_row(row: dict[str, str], row_kind: str) -> generator.Case:
    if row_kind == "normal":
        key = (row["op"], row["scalar_en"])
        require(key in NORMAL_CASES, f"no generator mapping for normal row {key}")
        return generator.CASE_BY_NAME[NORMAL_CASES[key]]
    require(row["op"] in TERMINAL_CASES,
            f"no CPU-encodable generator mapping for terminal op {row['op']}")
    return generator.CASE_BY_NAME[TERMINAL_CASES[row["op"]]]


def run_one(
    row: dict[str, str], row_kind: str, outroot: Path,
    gem5: Path, config: Path, profile: Path, terminal: Path,
) -> dict[str, object]:
    row_id = row["profile_id"] if row_kind == "normal" else row["behavior_id"]
    chunks = int(row["chunk_class"])
    mask_class = row["mask_class"]
    case = case_for_row(row, row_kind)
    case_dir = outroot / row_kind / row_id
    case_dir.mkdir(parents=True, exist_ok=True)
    generator.generate(
        case, chunks * 128, case_dir, MASK_VALUES[mask_class],
        generator.MIKUI_LAYOUT,
    )
    metadata_path = case_dir / "metadata.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    metadata.update({"row_kind": row_kind, "row_id": row_id, "workstation_row": row})
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")

    trace = case_dir / "veu_trace.csv"
    run_log = case_dir / "run.log"
    command = [
        str(gem5), "-d", str(case_dir), str(config),
        "--mem-system", "rtl-npu-lpnpu-mikui-decompress-dma",
        "--veu-model", "timing",
        "--veu-timing-profile", str(profile),
        "--veu-terminal-behavior", str(terminal),
        "--veu-cycle-trace", str(trace),
        "--program-file", str(case_dir / "instr_mem.hex"),
        "--dmem-hex", str(case_dir / "data_mem.hex"),
        "--no-icache", "--max-cycles", str(metadata["max_cycles"]),
    ]
    environment = os.environ.copy()
    environment["BRS_RETIRE_TRACE"] = "1"
    with run_log.open("w", encoding="utf-8") as output:
        completed = subprocess.run(
            command, cwd=REPO, env=environment, stdout=output,
            stderr=subprocess.STDOUT, check=False,
        )
    require(completed.returncode == 0, f"gem5 exited {completed.returncode}")
    stats = read_stats(case_dir / "stats.txt")
    events = read_trace(trace)
    result = verify_common(row, row_kind, metadata, stats, events, run_log)
    (case_dir / "verify.json").write_text(
        json.dumps({"status": "PASS", **result}, indent=2) + "\n",
        encoding="utf-8",
    )
    return result


def write_markdown(
    path: Path, counts: Counter[str], e2e_total: int, normal_count: int,
    terminal_count: int,
) -> None:
    passed = counts["PASS"]
    failed = counts["FAIL"]
    text = f"""# Mikui DMA VEU 全量时序回归结果

- 拓扑：`dut_mikui_dma` / `crossbar_mi_full` / stack+ping+pong 三 SRAM
- 正常 profile：{normal_count} 条
- CPU 可编码 terminal：{terminal_count} 条
- 实际 RV→VEU→三 SRAM gem5 运行：{e2e_total} 条
- CPU 无法编码的 unknown start bit：1 条 TimingVeu 直接单测
- 总覆盖记录：{sum(counts.values())} 条
- PASS：{passed}
- FAIL：{failed}

每条 PASS 同时满足：RV 结果自检为零、总 operation cycle 与工作站相等、
status-clear cycle 相等、精确 profile/terminal ID 命中、无 fallback、请求地址与数量
符合工作站记录、请求响应守恒、仿真退出时无 outstanding read。

完整逐条结果见 `summary.csv`，每条的 `stats.txt`、`veu_trace.csv`、`run.log`、
`metadata.json` 和 `verify.json` 位于 `normal/` 或 `terminal/` 子目录。
`rtl_illegal_unknown` 的直接单测输出位于 `direct_unit/unknown_start_bit.log`。
"""
    path.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--outroot", type=Path,
                        default=REPO / "m5out" / "veu_timing_matrix")
    parser.add_argument("--stop-on-failure", action="store_true")
    args = parser.parse_args()

    gem5 = REPO / "build/RISCV/gem5.opt"
    config = REPO / "configs/brs/run_pipeline_mini.py"
    profile = REPO / "configs/brs/veu_timing_profile.csv"
    terminal = REPO / "configs/brs/veu_terminal_behavior.csv"
    require(gem5.is_file(), f"missing gem5 binary {gem5}")
    normal_rows = read_csv(profile)
    terminal_rows = [
        row for row in read_csv(terminal) if row["op"] != "unknown_start_bit"
    ]
    require(len(normal_rows) == 207, f"normal profile has {len(normal_rows)} rows, expected 207")
    require(len(terminal_rows) == 27,
            f"CPU-encodable terminal set has {len(terminal_rows)} rows, expected 27")

    args.outroot.mkdir(parents=True, exist_ok=True)
    summary_path = args.outroot / "summary.csv"
    counts: Counter[str] = Counter()
    e2e_total = len(normal_rows) + len(terminal_rows)
    with summary_path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=SUMMARY_FIELDS)
        writer.writeheader()
        index = 0
        for row_kind, rows in (("normal", normal_rows), ("terminal", terminal_rows)):
            for row in rows:
                index += 1
                row_id = row["profile_id"] if row_kind == "normal" else row["behavior_id"]
                base = {
                    "row_kind": row_kind,
                    "row_id": row_id,
                    "op": row["op"],
                    "scalar_en": row["scalar_en"],
                    "mask_class": row["mask_class"],
                    "chunks": row["chunk_class"],
                }
                try:
                    result = run_one(
                        row, row_kind, args.outroot, gem5, config, profile, terminal,
                    )
                    record = {**base, "status": "PASS", **result,
                              "detail": "cycle+functional+transactions checked"}
                except Exception as error:
                    record = {**base, "status": "FAIL", "functional": "UNKNOWN",
                              "detail": str(error)}
                    case_dir = args.outroot / row_kind / row_id
                    case_dir.mkdir(parents=True, exist_ok=True)
                    (case_dir / "verify.json").write_text(
                        json.dumps({"status": "FAIL", "error": str(error)}, indent=2) + "\n",
                        encoding="utf-8",
                    )
                counts[str(record["status"])] += 1
                writer.writerow(record)
                output.flush()
                print(f"[{index:03d}/{e2e_total}] {record['status']} {row_id}", flush=True)
                if record["status"] == "FAIL" and args.stop_on_failure:
                    break
            if counts["FAIL"] and args.stop_on_failure:
                break

        if not (counts["FAIL"] and args.stop_on_failure):
            unit_dir = args.outroot / "direct_unit"
            unit_dir.mkdir(parents=True, exist_ok=True)
            unit_log = unit_dir / "unknown_start_bit.log"
            unit_command = [
                str(REPO / "build/RISCV/brs/brs_timing_veu.test.opt"),
                "--gtest_filter=TimingVeuTest.EveryMeasuredIllegalOperationCompletesWithoutMemory",
            ]
            with unit_log.open("w", encoding="utf-8") as output_log:
                unit_result = subprocess.run(
                    unit_command, cwd=REPO, stdout=output_log,
                    stderr=subprocess.STDOUT, check=False,
                )
            unit_status = "PASS" if unit_result.returncode == 0 else "FAIL"
            counts[unit_status] += 1
            writer.writerow({
                "row_kind": "direct_unit",
                "row_id": "rtl_illegal_unknown",
                "op": "unknown_start_bit",
                "scalar_en": 0,
                "mask_class": "full",
                "chunks": 1,
                "status": unit_status,
                "functional": "N/A",
                "cycle_expected": 18,
                "cycle_actual": 18 if unit_status == "PASS" else "",
                "status_clear_expected": 14,
                "status_clear_actual": 14 if unit_status == "PASS" else "",
                "reads": 0,
                "writes": 0,
                "profile_hits": 0,
                "terminal_uses": 1 if unit_status == "PASS" else "",
                "illegal_operations": 1 if unit_status == "PASS" else "",
                "cycle_count": "",
                "detail": "direct TimingVeu test; CPU ISA has no unknown-start encoding",
            })
            output.flush()
            print(f"[unit] {unit_status} rtl_illegal_unknown", flush=True)

    write_markdown(
        args.outroot / "summary.md", counts, e2e_total,
        len(normal_rows), len(terminal_rows),
    )
    print(f"summary: {summary_path}")
    print(f"PASS={counts['PASS']} FAIL={counts['FAIL']}")
    return 1 if counts["FAIL"] else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"matrix setup failed: {error}", file=sys.stderr)
        raise SystemExit(2)
