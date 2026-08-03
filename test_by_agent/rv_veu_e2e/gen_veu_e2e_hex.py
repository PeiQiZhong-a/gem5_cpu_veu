#!/usr/bin/env python3
"""Generate one self-checking RV-to-VEU timing-model E2E program.

Each generated image configures one timing-profile operation, polls VEUSTATUS,
then reads every result word back through the RV pipeline.  The program ORs
all word mismatches into x10; a passing program retires a final x10 value of
zero before ebreak.
"""

from __future__ import annotations

import argparse
import csv
import json
import struct
from dataclasses import dataclass, replace
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
SRC3 = 0x900


@dataclass(frozen=True)
class MemoryLayout:
    data_base: int
    src1: int
    src2: int
    src3: int
    dest: int
    expected: int


LOCAL_LAYOUT = MemoryLayout(0, SRC1, SRC2, SRC3, DEST, EXPECTED)
DUT_KUI_LAYOUT = MemoryLayout(
    0x29120000,
    0x29120020,
    0x29120120,
    0x29120220,
    0x29120420,
    0x29120620,
)


@dataclass(frozen=True)
class Case:
    name: str
    op: str
    function7: int
    source_set: str
    config: int
    scalar: int = 0
    reduction: str = ""
    illegal: bool = False
    mask: int = 0xFFFFFFFF


# The first 16 cases retain the original matrix.  The final six exercise
# operations that exist in the current RTL data path but have no RTL timing
# capture, and therefore must report default timing.
CASES = (
    Case("vadd_vector", "vadd", 0, "src1+src2", 0x100),
    Case("vadd_scalar", "vadd", 0, "src2", 0x900, 0x13131313),
    Case("vsub", "vsub", 1, "src1+src2", 0x100),
    Case("vmin", "vmin", 2, "src1+src2", 0x100),
    Case("vmax", "vmax", 3, "src1+src2", 0x100),
    Case("vredmin", "vredmin", 4, "src1", 0x100, reduction="min"),
    Case("vredmax", "vredmax", 5, "src1", 0x100, reduction="max"),
    Case("vand", "vand", 6, "src1+src2", 0x100),
    Case("vor", "vor", 7, "src1+src2", 0x100),
    Case("vxor", "vxor", 8, "src1+src2", 0x100),
    Case("vmv", "vmv", 11, "src1", 0x100),
    Case("vssrl_scalar", "vssrl", 12, "src2", 0x900, 3),
    Case("vssra_scalar", "vssra", 13, "src2", 0x900, 3),
    Case("vnclip_scalar", "vnclip", 14, "src2", 0x800, 0x00280005),
    Case("vredsum", "vredsum", 16, "src1", 0x100, reduction="sum"),
    Case("vmul", "vmul", 20, "src1+src2", 0x100),
    Case("vslideup_scalar", "vslideup", 9, "src2", 0x900, 3),
    Case("vslidedown_scalar", "vslidedown", 10, "src2", 0x900, 3),
    Case(
        "vwredsum", "vwredsum", 15, "src1+src2", 0x100,
        reduction="widen_sum", illegal=True,
    ),
    Case("vmsub", "vmsub", 0, "src1+src2+src3", 0x100),
    Case("vmac", "vmac", 1, "src1+src2+src3", 0x100),
    Case(
        "vmulh", "vmulh", 22, "src1+src2", 0x100, illegal=True,
    ),
)
CASE_BY_NAME = {case.name: case for case in CASES}
CAPTURED_CASE_BY_OP = {
    "vadd_vector": CASE_BY_NAME["vadd_vector"],
    "vadd_scalar": CASE_BY_NAME["vadd_scalar"],
    "vsub": CASE_BY_NAME["vsub"],
    "vmin": CASE_BY_NAME["vmin"],
    "vmax": CASE_BY_NAME["vmax"],
    "vand": CASE_BY_NAME["vand"],
    "vor": CASE_BY_NAME["vor"],
    "vxor": CASE_BY_NAME["vxor"],
    "vredsum": CASE_BY_NAME["vredsum"],
    "vmul": CASE_BY_NAME["vmul"],
    "vssrl": CASE_BY_NAME["vssrl_scalar"],
    "vssra": CASE_BY_NAME["vssra_scalar"],
    "vnclip": CASE_BY_NAME["vnclip_scalar"],
    "vmv": CASE_BY_NAME["vmv"],
    "vslideup": CASE_BY_NAME["vslideup_scalar"],
    "vslidedown": CASE_BY_NAME["vslidedown_scalar"],
    "vredmin": CASE_BY_NAME["vredmin"],
    "vredmax": CASE_BY_NAME["vredmax"],
    "vmac": CASE_BY_NAME["vmac"],
    "vmsub": CASE_BY_NAME["vmsub"],
    "vwredsum": CASE_BY_NAME["vwredsum"],
    "vmulh": CASE_BY_NAME["vmulh"],
}


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


def encode_three_source(
    rs3: int, function3: int, rd: int, rs1: int, rs2: int,
) -> int:
    return (
        (rs3 << 27)
        | (1 << 25)
        | (rs2 << 20)
        | (rs1 << 15)
        | (function3 << 12)
        | (rd << 7)
        | 0x2B
    )


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


def saturate_signed_byte(value: int) -> int:
    return min(max(value, -128), 127) & 0xFF


def make_sources(
    byte_count: int, rtl_vadd: bool = False,
) -> tuple[bytearray, bytearray, bytearray]:
    if rtl_vadd:
        return (
            bytearray([0xFD]) * byte_count,
            bytearray([0x01]) * byte_count,
            bytearray([0x00]) * byte_count,
        )
    # Both signed and unsigned patterns contain non-trivial boundary values.
    source1 = bytearray((0x83 + 17 * index) & 0xFF for index in range(byte_count))
    source2 = bytearray((0xF2 - 11 * index) & 0xFF for index in range(byte_count))
    source3 = bytearray((0x21 + 7 * index) & 0xFF for index in range(byte_count))
    return source1, source2, source3


def expected_result(
    case: Case, source1: bytearray, source2: bytearray, source3: bytearray,
) -> bytearray:
    result = bytearray(len(source1))
    for index, (a, b, c) in enumerate(zip(source1, source2, source3)):
        if case.name == "vadd_vector":
            value = saturate_signed_byte(signed_byte(a) + signed_byte(b))
        elif case.name == "vadd_scalar":
            scalar_byte = (case.scalar >> ((index % 4) * 8)) & 0xFF
            value = saturate_signed_byte(
                signed_byte(scalar_byte) + signed_byte(b)
            )
        elif case.name == "vsub":
            value = saturate_signed_byte(signed_byte(a) - signed_byte(b))
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
        elif case.name in {"vmul", "vmulh"}:
            value = saturate_signed_byte(signed_byte(a) * signed_byte(b))
        elif case.name == "vmac":
            value = saturate_signed_byte(
                signed_byte(a) * signed_byte(b) + signed_byte(c)
            )
        elif case.name == "vmsub":
            value = saturate_signed_byte(
                signed_byte(a) * signed_byte(b) - signed_byte(c)
            )
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
        result[:4] = struct.pack(
            "<I", sum(signed_byte(value) for value in source1) & 0xFFFFFFFF
        )
    elif case.reduction == "min":
        result[0] = min(source1, key=signed_byte)
    elif case.reduction == "max":
        result[0] = max(source1, key=signed_byte)
    elif case.reduction == "widen_sum":
        accumulator = 0
        for offset in range(0, len(source1), 32):
            accumulator += sum(
                signed_byte(value) for value in source1[offset:offset + 32]
            )
            result[offset:offset + 4] = struct.pack(
                "<I", accumulator & 0xFFFFFFFF
            )

    if case.name == "vslideup_scalar":
        shift = case.scalar & 0x1F
        result[:] = bytes(shift) + source2[:len(source2) - shift]
    elif case.name == "vslidedown_scalar":
        shift = case.scalar & 0x1F
        result[:] = source2[shift:] + bytes(shift)
    return result


def write_word_hex(path: Path, words: list[int]) -> None:
    path.write_text("".join(f"{word:08x}\n" for word in words), encoding="ascii")


def write_byte_hex(path: Path, data: bytearray) -> None:
    lines = []
    for offset in range(0, len(data), 16):
        lines.append(" ".join(f"{byte:02x}" for byte in data[offset:offset + 16]))
    path.write_text("\n".join(lines) + "\n", encoding="ascii")


def source_bases(case: Case, layout: MemoryLayout = LOCAL_LAYOUT) -> list[int]:
    if case.source_set == "src1+src2":
        return [layout.src1, layout.src2]
    if case.source_set == "src1":
        return [layout.src1]
    if case.source_set == "src2":
        return [layout.src2]
    if case.source_set == "src1+src2+src3":
        return [layout.src1, layout.src2, layout.src3]
    raise ValueError(f"unsupported source set: {case.source_set}")


def build_program(
    case: Case, vlen: int, expected: bytearray,
    layout: MemoryLayout = LOCAL_LAYOUT,
) -> list[int]:
    scalar_enabled = bool(case.config & 0x800)
    source1_register = case.scalar if scalar_enabled else layout.src1
    source2_register = layout.src2 if case.source_set != "src1" else 0

    words: list[int] = []
    words += emit_load_imm(1, source1_register)
    words += emit_load_imm(2, source2_register)
    words += emit_load_imm(3, layout.dest)
    words += emit_load_imm(4, case.config)
    words += emit_load_imm(5, vlen)
    words += emit_load_imm(6, case.mask)
    if case.source_set == "src1+src2+src3":
        words += emit_load_imm(14, layout.src3)
    words += [
        encode_vsetcsr(4, VEUCFG),
        encode_vsetcsr(3, VEUWADDR),
        encode_vsetcsr(5, VEUVLEN),
        encode_vsetcsr(6, VEUMASK),
        (
            encode_three_source(14, case.function7, 7, 1, 2)
            if case.source_set == "src1+src2+src3"
            else encode_vector(case.function7, 7, 1, 2)
        ),
        encode_vgetcsr(11, VEUSTATUS),
        encode_andi(11, 11, 1),
        encode_bne(11, 0, -8),
    ]
    words += emit_load_imm(8, layout.dest)
    words += emit_load_imm(9, layout.expected)
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


def build_metadata(
    case: Case, vlen: int, layout: MemoryLayout = LOCAL_LAYOUT,
) -> dict[str, object]:
    chunks = vlen // 256
    bases = source_bases(case, layout)
    reads = [base + chunk * 32 for base in bases for chunk in range(chunks)]
    writes = [layout.dest] if case.reduction in {"sum", "min", "max"} else [
        layout.dest + chunk * 32 for chunk in range(chunks)
    ]
    if case.mask == 0:
        writes = []
    uses_rtl_timing = True
    return {
        "case": case.name,
        "op": case.op,
        "source_set": case.source_set,
        "vlen": vlen,
        "chunks": chunks,
        "expected_reads": len(reads),
        "expected_writes": len(writes),
        "read_addresses": reads,
        "write_addresses": writes,
        "expected_base": layout.expected,
        "data_base": layout.data_base,
        "config": case.config,
        "mask": case.mask,
        "expected_timing_source": "rtl_sim" if uses_rtl_timing else "default",
        "expected_control_timing_source":
            "rtl_sim" if uses_rtl_timing else "default",
        "expected_profile_hits": 1 if uses_rtl_timing else 0,
        "expected_profile_fallbacks": 0 if uses_rtl_timing else 1,
        "expected_illegal_operations": 1 if case.illegal else 0,
        "max_cycles": 10000,
    }


def generate(
    case: Case, vlen: int, outdir: Path,
    layout: MemoryLayout = LOCAL_LAYOUT,
) -> None:
    outdir.mkdir(parents=True, exist_ok=True)
    byte_count = vlen // 8
    rtl_vadd = layout == DUT_KUI_LAYOUT
    if rtl_vadd and case.name != "vadd_vector":
        raise ValueError("dut-kui layout currently supports only vadd_vector")
    if rtl_vadd:
        case = replace(case, config=0x700)
    source1, source2, source3 = make_sources(byte_count, rtl_vadd=rtl_vadd)
    functional_result = expected_result(case, source1, source2, source3)
    expected = bytearray([0xA5]) * byte_count
    if case.reduction in {"sum", "min", "max"}:
        # Reduction policy is final_only: the last token writes one 32-byte
        # result chunk at DEST. Later destination chunks retain old memory.
        expected[:32] = functional_result[:32]
    else:
        expected = functional_result

    def offset(address: int) -> int:
        return address - layout.data_base

    memory_size = max(
        offset(layout.expected) + byte_count,
        offset(layout.src3) + byte_count,
    )
    memory = bytearray(memory_size)
    memory[offset(layout.src1):offset(layout.src1) + byte_count] = source1
    memory[offset(layout.src2):offset(layout.src2) + byte_count] = source2
    memory[offset(layout.src3):offset(layout.src3) + byte_count] = source3
    memory[offset(layout.dest):offset(layout.dest) + byte_count] = \
        bytes([0xA5]) * byte_count
    memory[offset(layout.expected):offset(layout.expected) + byte_count] = expected

    write_word_hex(
        outdir / "instr_mem.hex", build_program(case, vlen, expected, layout)
    )
    write_byte_hex(outdir / "data_mem.hex", memory)
    (outdir / "metadata.json").write_text(
        json.dumps(build_metadata(case, vlen, layout), indent=2) + "\n",
        encoding="utf-8",
    )


def captured_vector(text: str) -> bytes:
    """Convert the RTL trace's MSB-first 256-bit value to SRAM byte order."""
    if not text:
        return bytes(32)
    raw = bytes.fromhex(text)
    if len(raw) != 32:
        raise ValueError(f"captured vector has {len(raw)} bytes, expected 32")
    return raw[::-1]


def captured_rows(path: Path, test: str) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as input_file:
        reader = csv.DictReader(input_file)
        case_field = "test" if "test" in (reader.fieldnames or []) else "case_id"
        rows = [row for row in reader if row[case_field] == test]
    if not rows:
        raise ValueError(f"RTL test not found in {path}: {test}")
    aliases = {
        "case_id": "test",
        "operation": "op",
        "requested_vlen": "vlen",
        "rtl_result": "result",
    }
    for row in rows:
        for source, destination in aliases.items():
            if destination not in row and source in row:
                row[destination] = row[source]
    rows.sort(key=lambda row: int(row["chunk"], 0))
    if [int(row["chunk"], 0) for row in rows] != list(range(len(rows))):
        raise ValueError(f"RTL test has non-contiguous chunks: {test}")
    return rows


def generate_captured(
    vectors: Path, test: str, outdir: Path,
    layout: MemoryLayout = DUT_KUI_LAYOUT,
) -> None:
    rows = captured_rows(vectors, test)
    first = rows[0]
    scalar_enabled = bool(int(first["scalar_en"], 0))
    case_key = (
        "vadd_scalar" if scalar_enabled else "vadd_vector"
    ) if first["op"] == "vadd" else first["op"]
    if case_key not in CAPTURED_CASE_BY_OP:
        raise ValueError(f"unsupported captured RTL operation: {case_key}")
    case = replace(
        CAPTURED_CASE_BY_OP[case_key],
        config=int(first["config"], 16),
        scalar=int(first["scalar_value"], 16),
        mask=int(first["mask"], 16),
    )
    vlen = int(first["vlen"], 0)
    byte_count = vlen // 8
    if len(rows) * 32 != byte_count:
        raise ValueError(
            f"RTL test {test} has {len(rows)} chunks for VLEN={vlen}"
        )

    source1 = bytearray(byte_count)
    source2 = bytearray(byte_count)
    source3 = bytearray(byte_count)
    expected = bytearray([0xA5]) * byte_count
    for row in rows:
        for field in (
            "op", "config", "mode", "scalar_en", "scalar_value", "vlen", "mask",
        ):
            if row[field] != first[field]:
                raise ValueError(f"RTL test {test} changes {field} across chunks")
        chunk = int(row["chunk"], 0)
        begin = chunk * 32
        end = begin + 32
        source1[begin:end] = captured_vector(row["src1"])
        source2[begin:end] = captured_vector(row["src2"])
        source3[begin:end] = captured_vector(row["src3"])
        result = captured_vector(row["result"])
        strobe = int(row["wstrb"], 16)
        destination_begin = 0 if case.reduction in {"sum", "min", "max"} \
            else begin
        for byte in range(32):
            if strobe & (1 << byte):
                expected[destination_begin + byte] = result[byte]

    def offset(address: int) -> int:
        return address - layout.data_base

    memory_size = max(
        offset(layout.expected) + byte_count,
        offset(layout.src3) + byte_count,
    )
    memory = bytearray(memory_size)
    memory[offset(layout.src1):offset(layout.src1) + byte_count] = source1
    memory[offset(layout.src2):offset(layout.src2) + byte_count] = source2
    memory[offset(layout.src3):offset(layout.src3) + byte_count] = source3
    memory[offset(layout.dest):offset(layout.dest) + byte_count] = \
        bytes([0xA5]) * byte_count
    memory[offset(layout.expected):offset(layout.expected) + byte_count] = expected

    outdir.mkdir(parents=True, exist_ok=True)
    write_word_hex(
        outdir / "instr_mem.hex", build_program(case, vlen, expected, layout)
    )
    write_byte_hex(outdir / "data_mem.hex", memory)
    metadata = build_metadata(case, vlen, layout)
    captured_writes = [
        row for row in rows if int(row["wstrb"], 16) != 0
    ]
    metadata["expected_writes"] = len(captured_writes)
    metadata["write_addresses"] = [
        layout.dest
        if case.reduction in {"sum", "min", "max"}
        else layout.dest + int(row["chunk"], 0) * 32
        for row in captured_writes
    ]
    metadata.update({
        "case": test,
        "rtl_test": test,
        "rtl_vectors": str(vectors),
        "expected_timing_source": "rtl_sim",
        "expected_profile_hits": 1,
        "expected_profile_fallbacks": 0,
    })
    (outdir / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=sorted(CASE_BY_NAME))
    parser.add_argument("--vlen", type=int, choices=(256, 512, 1024, 2048))
    parser.add_argument("--outdir", type=Path)
    parser.add_argument(
        "--rtl-vectors", type=Path,
        help="Captured RTL veu_functional_vectors.csv input",
    )
    parser.add_argument("--rtl-test", help="Exact captured RTL test ID")
    parser.add_argument("--list-rtl-tests", action="store_true")
    parser.add_argument(
        "--layout", choices=("local", "dut-kui"), default="local",
        help="Architectural data address layout; default preserves existing tests",
    )
    parser.add_argument("--list-cases", action="store_true")
    args = parser.parse_args()

    if args.list_cases:
        for case in CASES:
            print(case.name)
        return
    if args.list_rtl_tests:
        if args.rtl_vectors is None:
            parser.error("--list-rtl-tests requires --rtl-vectors")
        with args.rtl_vectors.open(newline="", encoding="utf-8") as input_file:
            reader = csv.DictReader(input_file)
            case_field = (
                "test"
                if "test" in (reader.fieldnames or [])
                else "case_id"
            )
            print("\n".join(dict.fromkeys(
                row[case_field] for row in reader
            )))
        return
    if args.rtl_test is not None:
        if args.rtl_vectors is None or args.outdir is None:
            parser.error("--rtl-test requires --rtl-vectors and --outdir")
        generate_captured(args.rtl_vectors, args.rtl_test, args.outdir)
        return
    if args.case is None or args.vlen is None or args.outdir is None:
        parser.error("--case, --vlen, and --outdir are required unless --list-cases is used")
    layout = DUT_KUI_LAYOUT if args.layout == "dut-kui" else LOCAL_LAYOUT
    generate(CASE_BY_NAME[args.case], args.vlen, args.outdir, layout)


if __name__ == "__main__":
    main()
