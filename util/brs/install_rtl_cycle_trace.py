#!/usr/bin/env python3
"""Idempotently install the canonical trace monitor in aerith_tb_top.sv."""

from __future__ import annotations

import argparse
from pathlib import Path


MARKER = "// Canonical CPU cycle trace"
ANCHOR = "    // Test Sequence"


def install(top: Path, monitor_path: Path, check_only: bool) -> bool:
    raw = top.read_bytes()
    text = raw.decode("utf-8")
    newline = "\r\n" if b"\r\n" in raw else "\n"
    monitor = monitor_path.read_text(encoding="utf-8").replace("\n", newline)
    anchor = ANCHOR
    position = text.find(anchor)
    if position < 0:
        raise ValueError(f"cannot find test-sequence anchor in {top}")

    # Insert before the separator belonging to Test Sequence, not between its
    # separator and title.
    separator = "    //" + "=" * 72 + newline
    separator_position = text.rfind(separator, 0, position)
    if separator_position < 0:
        raise ValueError(f"cannot find separator before Test Sequence in {top}")
    marker_position = text.find(MARKER)
    if marker_position >= 0:
        old_start = text.rfind(separator, 0, marker_position)
        if old_start < 0:
            raise ValueError(f"cannot find installed monitor start in {top}")
        updated = text[:old_start] + monitor + text[separator_position:]
        past_action = "updated"
    else:
        updated = text[:separator_position] + monitor + text[separator_position:]
        past_action = "installed"

    if updated == text:
        print(f"RTL cycle trace monitor already current: {top}")
        return False
    if check_only:
        print(f"RTL cycle trace monitor can be {past_action}: {top}")
        return True

    top.write_bytes(updated.encode("utf-8"))
    print(f"RTL cycle trace monitor {past_action}: {top}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("rtl_top", type=Path,
                        help="path to aerith_tb_top.sv")
    parser.add_argument("--check", action="store_true",
                        help="validate insertion without writing")
    args = parser.parse_args()
    monitor = Path(__file__).with_name("rtl_cycle_trace_monitor.svinc")
    try:
        install(args.rtl_top, monitor, args.check)
    except (OSError, UnicodeError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
