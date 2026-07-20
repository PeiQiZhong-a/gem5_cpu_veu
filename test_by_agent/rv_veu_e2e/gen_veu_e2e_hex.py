#!/usr/bin/env python3
"""Generate one self-checking RV-to-VEU timing-model E2E program.

Each generated image configures one timing-profile operation, polls VEUSTATUS,
then reads every result word back through the RV pipeline.  The program ORs
all word mismatches into x10; a passing program retires a final x10 value of
zero before ebreak.
"""

from __future__ import annotations

import argparse
import json
import struct
from dataclasses import dataclass
from pathlib import Path


VEUCFG = 0x104
VEUSTATUS = 0x100
VEUWADDR = 0x103
VEUVLEN = 0x105
VEUMASK = 0x106

SRC1 = 0x100
SRC2 = 0x300
DEST = 0x500
EXPECTED = 0x700


@dataclass(frozen=True)
class Case:
    name: str
    op: str
    function7: int
    source_set: str
    config: int
    scalar: int = 0
    reduction: str = ""


# These are the 16 rows in configs/brs/veu_timing_profile.csv.  VADD has a
# vector and scalar row, so it appears as two independent program variants.
CASES = (
    Case("vadd_vector", "vadd", 0, "src1+src2", 0),
    Case("vadd_scalar", "vadd", 0, "src2", 0x800, 0x13),
    Case("vsub", "vsub", 1, "src1+src2", 0),
    Case("vmin", "vmin", 2, "src1+src2", 0x100),
    Case("vmax", "vmax", 3, "src1+src2", 0x100),
    Case("vredmin", "vredmin", 4, "src1", 0x100, reduction="min"),
    Case("vredmax", "vredmax", 5, "src1", 0x100, reduction="max"),
    Case("vand", "vand", 6, "src1+src2", 0),
    Case("vor", "vor", 7, "src1+src2", 0),
    Case("vxor", "vxor", 8, "src1+src2", 0),
    Case("vmv", "vmv", 11, "src1", 0),
    Case("vssrl_scalar", "vssrl", 12, "src2", 0x800, 3),
    Case("vssra_scalar", "vssra", 13, "src2", 0x900, 3),
    Case("vnclip_scalar", "vnclip", 14, "src2", 0x800, 0x00280005),
    Case("vredsum", "vredsum", 16, "src1", 0, reduction="sum"),
    Case("vmul", "vmul", 20, "src1+src2", 0),
)
CASE_BY_NAME = {case.name: case for case in CASES}


def encode_addi(rd: int, rs1: int, imm: int) -> int:
    return ((imm & 0xFFF) << 20) | (rs1 << 15) | (rd << 7) | 0x13


def encode_lui(rd: int, imm20: int) -> int:
    return ((imm20 & 0xFFFFF) << 12) | (rd << 7) | 0x37


def encode_lw(rd: int, rs1: int, imm: int) -> int:
    return ((imm & 0xFFF) << 20) | (rs1 << 15) | (0x2 << 12) | (rd << 7) | 0x03


def encode_andi(rd: int, rs1: int, imm: int) -> int:
    return ((imm & 0xFFF) << 20) | (rs1 << 15) | (0x7 << 12) | (rd << 7) | 0x13


def encode_xor(rd: int, rs1: int, rs2: int) -> int:
    return (rs2 << 20) | (rs1 << 15) | (0x4 << 12) | (rd << 7) | 0x33


def encode_or(rd: int, rs1: int, rs2: int) -> int:
    return (rs2 << 20) | (rs1 << 15) | (0x6 << 12) | (rd << 7) | 0x33


def encode_bne(rs1: int, rs2: int, imm: int) -> int:
    encoded = imm & 0x1FFF
    return (
        ((encoded >> 12) & 0x1) << 31
        | ((encoded >> 5) & 0x3F) << 25
        | (rs2 << 20)
        | (rs1 << 15)
        | (0x1 << 12)
        | ((encoded >> 1) & 0xF) << 8
        | ((encoded >> 11) & 0x1) << 7
        | 0x63
    )


def encode_vsetcsr(rs1: int, csr: int) -> int:
    return (csr << 20) | (rs1 << 15) | (0x2 << 12) | 0x0B


def encode_vgetcsr(rd: int, csr: int) -> int:
    return (csr << 20) | (0x3 << 12) | (rd << 7) | 0x0B


def encode_vector(function7: int, rd: int, rs1: int, rs2: int) -> int:
    return (function7 << 25) | (rs2 << 20) | (rs1 << 15) | (rd << 7) | 0x6B


def emit_load_imm(rd: int, value: int) -> list[int]:
    """Return LUI/ADDI code that writes the exact RV32 value to rd."""
    value &= 0xFFFFFFFF
    signed_value = value if value < 0x80000000 else value - 0x100000000
    if -2048 <= signed_value <= 2047:
        return [encode_addi(rd, 0, signed_value)]
    upper = (value + 0x800) >> 12
    lower = value - (upper << 12)
    if lower >= 0x800:
        lower -= 0x1000
    return [encode_lui(rd, upper), encode_addi(rd, rd, lower)]


def signed_byte(value: int) -> int:
    return value if value < 0x80 else value - 0x100


def make_sources(byte_count: int) -> tuple[bytearray, bytearray]:
    # Both signed and unsigned patterns contain non-trivial boundary values.
    source1 = bytearray((0x83 + 17 * index) & 0xFF for index in range(byte_count))
    source2 = bytearray((0xF2 - 11 * index) & 0xFF for index in range(byte_count))
    return source1, source2


def expected_result(case: Case, source1: bytearray, source2: bytearray) -> bytearray:
    result = bytearray(len(source1))
    for index, (a, b) in enumerate(zip(source1, source2)):
        if case.name == "vadd_vector":
            value = a + b
        elif case.name == "vadd_scalar":
            value = case.scalar + b
        elif case.name == "vsub":
            value = a - b
        elif case.name == "vmin":
            value = a if signed_byte(a) < signed_byte(b) else b
        elif case.name == "vmax":
            value = a if signed_byte(a) > signed_byte(b) else b
        elif case.name == "vand":
            value = a & b
        elif case.name == "vor":
            value = a | b
        elif case.name == "vxor":
            value = a ^ b
        elif case.name == "vmul":
            value = a * b
        elif case.name == "vmv":
            value = a
        elif case.name == "vssrl_scalar":
            value = b >> case.scalar
        elif case.name == "vssra_scalar":
            value = signed_byte(b) >> case.scalar
        elif case.name == "vnclip_scalar":
            minimum = case.scalar & 0xFF
            maximum = (case.scalar >> 16) & 0xFF
            value = min(max(b, minimum), maximum)
        else:
            continue
        result[index] = value & 0xFF

    if case.reduction == "sum":
        result[:4] = struct.pack("<I", sum(source1) & 0xFFFFFFFF)
    elif case.reduction == "min":
        result[0] = min(source1, key=signed_byte)
    elif case.reduction == "max":
        result[0] = max(source1, key=signed_byte)
    return result


def write_word_hex(path: Path, words: list[int]) -> None:
    path.write_text("".join(f"{word:08x}\n" for word in words), encoding="ascii")


def write_byte_hex(path: Path, data: bytearray) -> None:
    lines = []
    for offset in range(0, len(data), 16):
        lines.append(" ".join(f"{byte:02x}" for byte in data[offset:offset + 16]))
    path.write_text("\n".join(lines) + "\n", encoding="ascii")


def source_bases(case: Case) -> list[int]:
    if case.source_set == "src1+src2":
        return [SRC1, SRC2]
    if case.source_set == "src1":
        return [SRC1]
    if case.source_set == "src2":
        return [SRC2]
    raise ValueError(f"unsupported source set: {case.source_set}")


def build_program(case: Case, vlen: int, expected: bytearray) -> list[int]:
    scalar_enabled = bool(case.config & 0x800)
    source1_register = case.scalar if scalar_enabled else SRC1
    source2_register = SRC2 if case.source_set != "src1" else 0

    words: list[int] = []
    words += emit_load_imm(1, source1_register)
    words += emit_load_imm(2, source2_register)
    words += emit_load_imm(3, DEST)
    words += emit_load_imm(4, case.config)
    words += emit_load_imm(5, vlen)
    words += emit_load_imm(6, 0xFFFFFFFF)
    words += [
        encode_vsetcsr(4, VEUCFG),
        encode_vsetcsr(3, VEUWADDR),
        encode_vsetcsr(5, VEUVLEN),
        encode_vsetcsr(6, VEUMASK),
        encode_vector(case.function7, 7, 1, 2),
        encode_vgetcsr(11, VEUSTATUS),
        encode_andi(11, 11, 1),
        encode_bne(11, 0, -8),
    ]
    words += emit_load_imm(8, DEST)
    words += emit_load_imm(9, EXPECTED)
    words += [
        encode_addi(10, 0, 0),
    ]
    words += emit_load_imm(11, len(expected) // 4)

    # ProgramImage currently holds 64 instructions.  A loop keeps the 2048-bit
    # case compact while still checking every destination word against an
    # independently generated DMEM golden image.
    words += [
        encode_lw(12, 8, 0),
        encode_lw(13, 9, 0),
        encode_xor(12, 12, 13),
        encode_or(10, 10, 12),
        encode_addi(8, 8, 4),
        encode_addi(9, 9, 4),
        encode_addi(11, 11, -1),
        encode_bne(11, 0, -28),
    ]
    words.append(0x00100073)  # ebreak
    return words


def build_metadata(case: Case, vlen: int) -> dict[str, object]:
    chunks = vlen // 256
    bases = source_bases(case)
    reads = [base + chunk * 32 for base in bases for chunk in range(chunks)]
    writes = [DEST] if case.reduction else [DEST + chunk * 32 for chunk in range(chunks)]
    return {
        "case": case.name,
        "op": case.op,
        "vlen": vlen,
        "chunks": chunks,
        "expected_reads": len(reads),
        "expected_writes": len(writes),
        "read_addresses": reads,
        "write_addresses": writes,
        "expected_base": EXPECTED,
        "max_cycles": 10000,
    }


def generate(case: Case, vlen: int, outdir: Path) -> None:
    outdir.mkdir(parents=True, exist_ok=True)
    byte_count = vlen // 8
    source1, source2 = make_sources(byte_count)
    functional_result = expected_result(case, source1, source2)
    expected = bytearray([0xA5]) * byte_count
    if case.reduction:
        # Reduction policy is final_only: the last token writes one 32-byte
        # result chunk at DEST. Later destination chunks retain old memory.
        expected[:32] = functional_result[:32]
    else:
        expected = functional_result

    memory = bytearray(EXPECTED + byte_count)
    memory[SRC1:SRC1 + byte_count] = source1
    memory[SRC2:SRC2 + byte_count] = source2
    memory[DEST:DEST + byte_count] = bytes([0xA5]) * byte_count
    memory[EXPECTED:EXPECTED + byte_count] = expected

    write_word_hex(outdir / "instr_mem.hex", build_program(case, vlen, expected))
    write_byte_hex(outdir / "data_mem.hex", memory)
    (outdir / "metadata.json").write_text(
        json.dumps(build_metadata(case, vlen), indent=2) + "\n",
        encoding="utf-8",
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=sorted(CASE_BY_NAME))
    parser.add_argument("--vlen", type=int, choices=(256, 2048))
    parser.add_argument("--outdir", type=Path)
    parser.add_argument("--list-cases", action="store_true")
    args = parser.parse_args()

    if args.list_cases:
        for case in CASES:
            print(case.name)
        return
    if args.case is None or args.vlen is None or args.outdir is None:
        parser.error("--case, --vlen, and --outdir are required unless --list-cases is used")
    generate(CASE_BY_NAME[args.case], args.vlen, args.outdir)


if __name__ == "__main__":
    main()
