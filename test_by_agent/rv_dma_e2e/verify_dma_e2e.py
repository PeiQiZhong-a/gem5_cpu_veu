#!/usr/bin/env python3
"""Verify independent Mikui decompression-DMA statistics."""

import argparse
from pathlib import Path


def parse_stats(path):
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) >= 2:
            try:
                values[fields[0]] = float(fields[1])
            except ValueError:
                pass
    return values


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-log", type=Path, required=True)
    parser.add_argument("--stats", type=Path, required=True)
    args = parser.parse_args()

    log = args.run_log.read_text(encoding="utf-8")
    values = parse_stats(args.stats)
    required = {
        "system.mikui_dma.readWords": 2,
        "system.mikui_dma.writeWords": 1,
        "system.mikui_dma.completedOperations": 1,
        "system.mikui_dma.decodeErrors": 0,
        "system.mikui_dma.irqAssertions": 1,
        "system.mikui_dma.outputChecksum": 0x2E342DF7,
    }
    errors = []
    if "PipelineMiniCPU completed test" not in log:
        errors.append("guest did not reach EBREAK")
    if "RTL testbench timeout" in log:
        errors.append("guest timed out")
    for name, expected in required.items():
        actual = values.get(name)
        if actual != expected:
            errors.append(f"{name}: expected {expected}, got {actual}")
    if values.get("system.mikui_dma.pioWrites", 0) < 4:
        errors.append("dt_dma register programming was not fully observed")

    if errors:
        raise SystemExit("DMA E2E FAIL: " + "; ".join(errors))
    print("DMA E2E PASS: independent device decoded 0,-1,1,-2")


if __name__ == "__main__":
    main()
