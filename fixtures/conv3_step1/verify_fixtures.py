#!/usr/bin/env python3
"""Verify Step 1 fixture provenance, SAU counts, and allowed image diffs."""

from __future__ import annotations

import argparse
import hashlib
import json
import tarfile
from pathlib import Path


WORD_COUNT = 65536
SAU_FUNCT7_TO_SLOT = {0: 1, 3: 2, 6: 3, 9: 4}
FIXTURE_NAMES = (
    "generated_legacy_control",
    "generated_four_ins_control_matched",
    "generated_four_ins_full_offload",
)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_words(path: Path) -> list[int]:
    tokens = path.read_bytes().split()
    if len(tokens) != WORD_COUNT:
        raise AssertionError(f"{path}: expected {WORD_COUNT} tokens, got {len(tokens)}")
    words = []
    for index, token in enumerate(tokens):
        text = token.decode("ascii")
        if len(text) != 8:
            raise AssertionError(f"{path}: token {index} is not 8 hex digits")
        words.append(int(text, 16))
    return words


def sau_counts(words: list[int]) -> dict[str, int]:
    counts = {f"msetins{slot}": 0 for slot in range(1, 5)}
    for word in words:
        if (word & 0x7F) != 0x6B or ((word >> 12) & 0x7) != 1:
            continue
        slot = SAU_FUNCT7_TO_SLOT.get((word >> 25) & 0x7F)
        if slot is not None:
            counts[f"msetins{slot}"] += 1
    return counts


def bytes_from_words(words: list[int]) -> bytes:
    return b"".join(word.to_bytes(4, "little") for word in words)


def assert_equal_outside(
    left: bytes, right: bytes, allowed_ranges: list[tuple[int, int]], label: str
) -> None:
    if len(left) != len(right):
        raise AssertionError(f"{label}: image lengths differ")
    allowed = [False] * len(left)
    for start, end in allowed_ranges:
        for index in range(start, end):
            if index < len(allowed):
                allowed[index] = True
    differences = [
        index for index, (lhs, rhs) in enumerate(zip(left, right))
        if lhs != rhs and not allowed[index]
    ]
    if differences:
        preview = ", ".join(f"0x{index:x}" for index in differences[:8])
        raise AssertionError(f"{label}: unexpected differences at {preview}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument(
        "--archive",
        type=Path,
        default=Path("/home/xch/workspace/kuiloong-conv2d-testcase2.tar.gz"),
    )
    args = parser.parse_args()
    root = args.root.resolve()
    archive_path = args.archive.resolve()
    with tarfile.open(archive_path, "r:gz") as archive:
        archive_memory = archive.extractfile("memory.hex")
        if archive_memory is None:
            raise SystemExit("archive is missing memory.hex")
        expected_memory = archive_memory.read()

    manifests = {}
    images = {}
    for name in FIXTURE_NAMES:
        fixture_dir = root / name
        manifest_path = fixture_dir / "manifest.json"
        if not manifest_path.is_file():
            raise SystemExit(f"missing generated fixture: {manifest_path}")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifests[name] = manifest
        instruction = fixture_dir / "instruction.hex"
        memory = fixture_dir / "memory.hex"
        words = read_words(instruction)
        instruction_bytes = bytes_from_words(words)
        images[name] = instruction_bytes

        observed = sau_counts(words)
        expected = manifest["fixture"]["sau_instruction_counts"]
        if observed != expected:
            raise AssertionError(f"{name}: SAU counts {observed} != {expected}")
        if sha256_bytes(instruction.read_bytes()) != \
                manifest["outputs"]["instruction_hex"]["sha256"]:
            raise AssertionError(f"{name}: instruction hash mismatch")
        if sha256_bytes(memory.read_bytes()) != \
                manifest["outputs"]["memory_hex"]["sha256"]:
            raise AssertionError(f"{name}: memory hash mismatch")
        if memory.read_bytes() != expected_memory:
            raise AssertionError(f"{name}: memory.hex differs from archive member")
        tail_pc = manifest["program_layout"]["shared_tail_pc"]
        if int.from_bytes(instruction_bytes[tail_pc:tail_pc + 4], "little") != \
                0x00100073:
            raise AssertionError(f"{name}: shared tail is not ebreak")

    base = manifests[FIXTURE_NAMES[0]]
    for name in FIXTURE_NAMES[1:]:
        if manifests[name]["archive"]["sha256"] != base["archive"]["sha256"]:
            raise AssertionError(f"{name}: archive provenance differs")
        if manifests[name]["conv"] != base["conv"]:
            raise AssertionError(f"{name}: Conv layout differs")
        if manifests[name]["program_layout"] != base["program_layout"]:
            raise AssertionError(f"{name}: program layout differs")

    padding = tuple(base["program_layout"]["padding_region"])
    config = tuple(base["program_layout"]["config_region"])
    assert_equal_outside(
        images["generated_legacy_control"],
        images["generated_four_ins_control_matched"],
        [config],
        "legacy vs control-matched",
    )
    assert_equal_outside(
        images["generated_four_ins_control_matched"],
        images["generated_four_ins_full_offload"],
        [padding],
        "control-matched vs full-offload",
    )

    if images["generated_four_ins_control_matched"][0x4000:0x5000] != \
            images["generated_four_ins_full_offload"][0x4000:0x5000]:
        raise AssertionError("new ABI config region differs between C and D")

    print("Step 1 fixture verification: PASS")
    for name in FIXTURE_NAMES:
        print(f"  {name}: {sau_counts(read_words(root / name / 'instruction.hex'))}")


if __name__ == "__main__":
    main()
