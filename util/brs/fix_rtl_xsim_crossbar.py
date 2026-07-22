#!/usr/bin/env python3
"""Give the two crossbar response pipelines separate SV loop variables."""

from __future__ import annotations

import argparse
from pathlib import Path


OLD_DECL = "    integer i;"
NEW_DECL = "    integer ibus_i;\n    integer dbus_i;"
DBUS_MARKER = "    // DBus response pipeline"


def fix(path: Path, check_only: bool) -> bool:
    raw = path.read_bytes()
    newline = "\r\n" if b"\r\n" in raw else "\n"
    text = raw.decode("utf-8")
    if "integer ibus_i;" in text and "integer dbus_i;" in text:
        print(f"crossbar xsim loop variables already fixed: {path}")
        return False
    if text.count(OLD_DECL) != 1:
        raise ValueError(f"expected one shared loop declaration in {path}")
    marker = text.find(DBUS_MARKER)
    if marker < 0:
        raise ValueError(f"cannot find DBus pipeline marker in {path}")

    text = text.replace(OLD_DECL, NEW_DECL.replace("\n", newline), 1)
    marker = text.find(DBUS_MARKER)
    ibus_part = text[:marker].replace("for (i =", "for (ibus_i =")
    ibus_part = ibus_part.replace("i < IBUS_RESP_DELAY", "ibus_i < IBUS_RESP_DELAY")
    ibus_part = ibus_part.replace("i > 0", "ibus_i > 0")
    ibus_part = ibus_part.replace("i = i + 1", "ibus_i = ibus_i + 1")
    ibus_part = ibus_part.replace("i = i - 1", "ibus_i = ibus_i - 1")
    ibus_part = ibus_part.replace("[i]", "[ibus_i]")
    ibus_part = ibus_part.replace("[i-1]", "[ibus_i-1]")

    dbus_part = text[marker:].replace("for (i =", "for (dbus_i =")
    dbus_part = dbus_part.replace("i < DBUS_RESP_DELAY", "dbus_i < DBUS_RESP_DELAY")
    dbus_part = dbus_part.replace("i > 0", "dbus_i > 0")
    dbus_part = dbus_part.replace("i = i + 1", "dbus_i = dbus_i + 1")
    dbus_part = dbus_part.replace("i = i - 1", "dbus_i = dbus_i - 1")
    dbus_part = dbus_part.replace("[i]", "[dbus_i]")
    dbus_part = dbus_part.replace("[i-1]", "[dbus_i-1]")
    updated = ibus_part + dbus_part

    required = (
        "for (ibus_i = 0; ibus_i < IBUS_RESP_DELAY; ibus_i = ibus_i + 1)",
        "for (ibus_i = IBUS_RESP_DELAY-1; ibus_i > 0; ibus_i = ibus_i - 1)",
        "for (dbus_i = 0; dbus_i < DBUS_RESP_DELAY; dbus_i = dbus_i + 1)",
        "for (dbus_i = DBUS_RESP_DELAY-1; dbus_i > 0; dbus_i = dbus_i - 1)",
    )
    if not all(fragment in updated for fragment in required):
        raise ValueError(f"crossbar loop structure did not match expectations in {path}")
    if check_only:
        print(f"crossbar xsim loop variables can be fixed: {path}")
        return True
    path.write_bytes(updated.encode("utf-8"))
    print(f"fixed crossbar xsim loop variables: {path}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("crossbar", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        fix(args.crossbar, args.check)
    except (OSError, UnicodeError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
