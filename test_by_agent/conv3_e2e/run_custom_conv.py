#!/usr/bin/env python3
"""Generate and optionally run one configurable four-instruction sau_n case.

The runner supports a small hand-written boot path and a ``toolchain-approx``
startup path.  The latter models the relevant guest-side setup from the
Kuiloong ``7_sau`` test: a shadow first-fit allocator with heap/header checks,
four malloc calls, source loads for input/weight/bias, and the mgetins4 idle
polls.  It remains an approximation rather than the compiler's real firmware
image.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import random
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Iterable


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
DEFAULT_OUTPUT_ROOT = SCRIPT_DIR / "custom_runs"

IMAGE_BYTES = 0x00040000
WORD_BYTES = 4
INSTRUCTION_TAIL_PC = 0x00005000
CONFIG_START_PC = 0x00004000

DEFAULT_DATA_BASE = 0x29120000
DEFAULT_DATA_SIZE = 0x00040000
DEFAULT_BANK_SIZE = 0x00010000
DEFAULT_BANK_COUNT = 4
DEFAULT_REAL_BANK_COUNT = 3

# t0=x5 is rs1, t1=x6 is rs2.  funct7 selects msetins1..4.
MSET_WORDS = {
    1: 0x0062906B,
    2: 0x0662906B,
    3: 0x0C62906B,
    4: 0x1262906B,
}

# mgetins4lsb with rd=t0.  The gem5 sau decoder follows Spirit's funct7
# numbering: Set4 is funct7=9 and Get4Lsb is funct7=10.
MGETINS4_LSB_WORD = 0x140012EB
TOOLCHAIN_APPROX_STARTUP = "toolchain-approx"
MINIMAL_STARTUP = "minimal"
ALLOCATOR_BYTES = 0x100
ALLOCATOR_ALIGNMENT = 16
ALLOCATOR_LIST_OFFSET = 0x00
ALLOCATOR_FREE_BLOCK_OFFSET = 0x10
ALLOCATOR_STATE_OFFSET = 0x20
ALLOCATOR_RECORD_BASE = 0x40
ALLOCATOR_RECORD_STRIDE = 0x10
ALLOCATOR_VIRTUAL_SLACK_HEADERS = 8
ALLOCATOR_CANARY = 0xDEADBEEF


def parse_int(value: str) -> int:
    return int(value, 0)


def checked_product(values: Iterable[int], label: str) -> int:
    result = 1
    for value in values:
        result *= value
        if result < 0:
            raise ValueError(f"{label} overflowed")
    return result


def ceil_div(value: int, divisor: int) -> int:
    return (value + divisor - 1) // divisor


def align_up(value: int, alignment: int) -> int:
    return ((value + alignment - 1) // alignment) * alignment


def allocator_true_size(size: int) -> int:
    """Return first_fit.cpp's allocation size in 16-byte header units."""
    aligned_size = align_up(size, ALLOCATOR_ALIGNMENT)
    return aligned_size // ALLOCATOR_ALIGNMENT + 1


def find_free_region(
    ranges: list[tuple[str, int, int]],
    data_base: int,
    data_end: int,
    size: int,
    alignment: int,
) -> int:
    occupied = sorted((base, end) for _, base, end in ranges)
    gaps: list[tuple[int, int]] = []
    cursor = data_base
    for base, end in occupied:
        if cursor < base:
            gaps.append((cursor, base))
        cursor = max(cursor, end)
    if cursor < data_end:
        gaps.append((cursor, data_end))
    for gap_start, gap_end in reversed(gaps):
        candidate = align_up(gap_start, alignment)
        if candidate + size <= gap_end:
            return candidate
    raise ValueError(
        "no free SRAM region is available for CPU tensor initialization sources"
    )


def tensor_ranges(config: dict[str, int]) -> list[tuple[str, int, int]]:
    return [
        ("input", config["input_base"], config["input_bytes"]),
        ("weight", config["weight_base"], config["weight_bytes"]),
        ("bias", config["bias_base"], config["bias_bytes"]),
        ("output", config["output_base"], config["output_bytes"]),
    ]


def validate_and_derive(args: argparse.Namespace) -> dict[str, int]:
    limits = (
        ("n", args.n, 1, 65535),
        ("channels", args.channels, 1, 63),
        ("height", args.height, 1, 65535),
        ("width", args.width, 1, 65535),
        ("out_channels", args.out_channels, 1, 16),
    )
    for name, value, minimum, maximum in limits:
        if not minimum <= value <= maximum:
            raise ValueError(f"{name} must be in [{minimum}, {maximum}]")
    if args.startup_model not in (MINIMAL_STARTUP, TOOLCHAIN_APPROX_STARTUP):
        raise ValueError(
            f"startup-model must be {MINIMAL_STARTUP} or "
            f"{TOOLCHAIN_APPROX_STARTUP}"
        )
    if args.padding not in (0, 1):
        raise ValueError("padding must be 0 or 1")
    if args.stride not in (1, 2):
        raise ValueError("stride must be 1 or 2")
    if not 0 <= args.cutbit <= 23:
        raise ValueError("cutbit must be in [0, 23]")

    padded_h = args.height + 2 * args.padding
    padded_w = args.width + 2 * args.padding
    if padded_h < 3 or padded_w < 3:
        raise ValueError("padding and input shape do not produce a 3x3 output")
    output_h = (padded_h - 3) // args.stride + 1
    output_w = (padded_w - 3) // args.stride + 1
    if args.width <= 16 and output_w > args.width:
        raise ValueError("sau_n requires output_w <= width when width <= 16")

    input_bytes = checked_product(
        (args.n, args.channels, args.height, args.width), "input size"
    )
    weight_bytes = checked_product(
        (args.channels, 3, 3, args.out_channels), "weight size"
    )
    bias_bytes = args.out_channels * 2
    output_bytes = checked_product(
        (args.n, args.out_channels, output_h, output_w), "output size"
    )

    if args.data_size <= 0 or args.data_size % WORD_BYTES != 0:
        raise ValueError("data-size must be a positive multiple of 4")
    if args.data_bank_size <= 0 or args.data_bank_count not in range(1, 5):
        raise ValueError("data-bank-size must be positive and bank-count in 1..4")
    if not 0 <= args.data_real_bank_count <= args.data_bank_count:
        raise ValueError("data-real-bank-count must be in 0..data-bank-count")
    if args.data_bank_size * args.data_bank_count != args.data_size:
        raise ValueError("data-size must equal data-bank-size * data-bank-count")
    if args.data_base < 0 or args.data_base + args.data_size > 1 << 32:
        raise ValueError("data window must fit in the 32-bit address space")

    real_capacity = args.data_bank_size * args.data_real_bank_count
    data_end = args.data_base + real_capacity
    ranges = tensor_ranges({
        "input_base": args.input_base,
        "input_bytes": input_bytes,
        "weight_base": args.weight_base,
        "weight_bytes": weight_bytes,
        "bias_base": args.bias_base,
        "bias_bytes": bias_bytes,
        "output_base": args.output_base,
        "output_bytes": output_bytes,
    })
    checked_ranges: list[tuple[str, int, int]] = []
    for name, base, size in ranges:
        if not 0 <= base < 1 << 32:
            raise ValueError(f"{name}_base must fit in 32 bits")
        end = base + size
        if end > 1 << 32:
            raise ValueError(f"{name} address range overflows 32 bits")
        if base < args.data_base or end > data_end:
            raise ValueError(
                f"{name} range 0x{base:x}..0x{end:x} is outside real SRAM "
                f"0x{args.data_base:x}..0x{data_end:x}"
            )
        checked_ranges.append((name, base, end))
    if args.bias_base & 1:
        raise ValueError("bias-base must be 2-byte aligned")
    for index, (left_name, left_base, left_end) in enumerate(checked_ranges):
        for right_name, right_base, right_end in checked_ranges[index + 1:]:
            if left_base < right_end and right_base < left_end:
                raise ValueError(f"{left_name} and {right_name} ranges overlap")

    allocator_base = None
    reserved_ranges = list(checked_ranges)
    if args.startup_model == TOOLCHAIN_APPROX_STARTUP:
        allocator_base = find_free_region(
            reserved_ranges,
            args.data_base,
            data_end,
            ALLOCATOR_BYTES,
            ALLOCATOR_ALIGNMENT,
        )
        reserved_ranges.append(
            ("allocator", allocator_base, allocator_base + ALLOCATOR_BYTES)
        )

    source_specs: list[tuple[str, int, int]] = []
    if args.input_init == "cpu" and args.startup_model == TOOLCHAIN_APPROX_STARTUP:
        source_specs.append(("input", input_bytes, 1))
    if args.weight_init == "cpu":
        source_specs.append(("weight", weight_bytes, 1))
    if args.bias_init == "cpu":
        source_specs.append(("bias", bias_bytes, 2))
    source_offsets: dict[str, int] = {}
    source_size = 0
    for name, size, alignment in source_specs:
        source_size = align_up(source_size, alignment)
        source_offsets[name] = source_size
        source_size += size
    source_size = align_up(source_size, WORD_BYTES) if source_size else 0
    source_base = None
    if source_size:
        source_base = find_free_region(
            reserved_ranges,
            args.data_base,
            data_end,
            source_size,
            WORD_BYTES,
        )

    allocation_sizes = (input_bytes, weight_bytes, bias_bytes, output_bytes)
    allocator_virtual_headers = (
        sum(allocator_true_size(size) for size in allocation_sizes)
        + ALLOCATOR_VIRTUAL_SLACK_HEADERS
    )

    rows_per_word = 16 // args.width if args.width <= 16 else 1
    spatial_words_per_channel = (
        ceil_div(args.height, rows_per_word)
        if args.width <= 16
        else args.height * ceil_div(args.width, 16)
    )
    a_rows = args.n * args.channels * spatial_words_per_channel
    b_rows = args.channels * 3 * 3
    d_rows = args.n * output_h * output_w
    scratchpad_rows = a_rows + b_rows + 2 + d_rows
    if scratchpad_rows > 4096:
        raise ValueError(
            "default sau_n shared scratchpad needs "
            f"{scratchpad_rows} rows; maximum is 4096"
        )

    return {
        "n": args.n,
        "c": args.channels,
        "h": args.height,
        "w": args.width,
        "oc": args.out_channels,
        "padding": args.padding,
        "stride": args.stride,
        "cutbit": args.cutbit,
        "out_h": output_h,
        "out_w": output_w,
        "input_bytes": input_bytes,
        "weight_bytes": weight_bytes,
        "bias_bytes": bias_bytes,
        "output_bytes": output_bytes,
        "real_capacity": real_capacity,
        "a_rows": a_rows,
        "b_rows": b_rows,
        "d_rows": d_rows,
        "scratchpad_rows": scratchpad_rows,
        "allocator_base": allocator_base,
        "allocator_bytes": ALLOCATOR_BYTES if allocator_base is not None else 0,
        "allocator_virtual_headers": (
            allocator_virtual_headers if allocator_base is not None else 0
        ),
        "startup_model": args.startup_model,
        "input_init_source_base": (
            None
            if source_base is None or "input" not in source_offsets
            else source_base + source_offsets["input"]
        ),
        "weight_init_source_base": (
            None
            if source_base is None or "weight" not in source_offsets
            else source_base + source_offsets["weight"]
        ),
        "bias_init_source_base": (
            None
            if source_base is None or "bias" not in source_offsets
            else source_base + source_offsets["bias"]
        ),
        "init_source_bytes": source_size,
    }


def payloads(args: argparse.Namespace, config: dict[str, int]) -> list[int]:
    return [
        args.input_base | (args.weight_base << 32),
        args.bias_base | (args.output_base << 32),
        (
            config["h"]
            | (config["w"] << 16)
            | (config["n"] << 32)
            | (config["c"] << 48)
            | (config["oc"] << 54)
        ),
        (
            1
            | (config["padding"] << 4)
            | ((config["stride"] - 1) << 5)
            | (config["cutbit"] << 6)
            | (3 << 11)
            | (1 << 31)
            | (0xC3 << 56)
        ),
    ]


def low32(value: int) -> int:
    return value & 0xFFFFFFFF


def high32(value: int) -> int:
    return (value >> 32) & 0xFFFFFFFF


def append_toolchain_approx_allocator(
    lines: list[str], args: argparse.Namespace, config: dict[str, int]
) -> None:
    allocator_base = config["allocator_base"]
    if allocator_base is None:
        raise ValueError("toolchain-approx startup lacks allocator storage")

    sentinel = allocator_base + ALLOCATOR_LIST_OFFSET
    free_block = allocator_base + ALLOCATOR_FREE_BLOCK_OFFSET
    state = allocator_base + ALLOCATOR_STATE_OFFSET
    heap_start = free_block
    heap_end = allocator_base + ALLOCATOR_BYTES
    lines.extend([
        "",
        "    # Shadow malloc_initial()/init_heap() from toolchain first-fit.",
        f"    li      t0, 0x{sentinel:08x}",
        f"    li      t6, 0x{state:08x}",
        "    lw      t5, 4(t6)",
        "    bnez    t5, allocator_init_done",
        f"    li      t1, 0x{heap_start:08x}",
        f"    li      t2, 0x{heap_end:08x}",
        f"    andi    t3, t1, {ALLOCATOR_ALIGNMENT - 1}",
        "    bnez    t3, allocator_init_fail",
        "    bgeu    t1, t2, allocator_init_fail",
        "    sw      t1, 8(t6)",
        "    sw      t2, 12(t6)",
        f"    li      t4, 0x{free_block:08x}",
        "    sw      t4, 0(t0)",
        "    sw      zero, 4(t0)",
        f"    li      t3, 0x{ALLOCATOR_CANARY:08x}",
        "    sw      t3, 8(t0)",
        "    sw      zero, 12(t0)",
        f"    li      t4, 0x{free_block:08x}",
        "    sw      t0, 0(t4)",
        f"    li      t5, {config['allocator_virtual_headers']}",
        "    sw      t5, 4(t4)",
        "    sw      t3, 8(t4)",
        "    sw      zero, 12(t4)",
        "    sw      t0, 0(t6)",
        "    li      t5, 1",
        "    sw      t5, 4(t6)",
        f"    li      t5, {config['allocator_virtual_headers']}",
        "    sw      t5, 16(t6)",
        "    sw      zero, 20(t6)",
        "    sw      zero, 24(t6)",
        "    sw      zero, 28(t6)",
        "    jal     zero, allocator_init_done",
        "allocator_init_fail:",
        "    sw      zero, 4(t6)",
        "allocator_init_done:",
        "",
        "    # Four guest ff_malloc calls: A, B, C and D.",
    ])
    allocations = (
        (
            "input",
            config["input_bytes"],
            args.input_base,
            ALLOCATOR_RECORD_BASE,
        ),
        (
            "weight",
            config["weight_bytes"],
            args.weight_base,
            ALLOCATOR_RECORD_BASE + ALLOCATOR_RECORD_STRIDE,
        ),
        (
            "bias",
            config["bias_bytes"],
            args.bias_base,
            ALLOCATOR_RECORD_BASE + 2 * ALLOCATOR_RECORD_STRIDE,
        ),
        (
            "output",
            config["output_bytes"],
            args.output_base,
            ALLOCATOR_RECORD_BASE + 3 * ALLOCATOR_RECORD_STRIDE,
        ),
    )
    for name, size, target, record_offset in allocations:
        lines.extend([
            f"    # malloc({name}, {size}) -> fixed tensor address",
            f"    li      a0, {size}",
            f"    li      a1, 0x{target:08x}",
            f"    li      a2, 0x{allocator_base + record_offset:08x}",
            "    jal     ra, ff_malloc_shadow",
        ])


def append_toolchain_approx_allocator_helper(
    lines: list[str], config: dict[str, int]
) -> None:
    allocator_base = config["allocator_base"]
    if allocator_base is None:
        raise ValueError("toolchain-approx startup lacks allocator storage")
    lines.extend([
        "",
        "    # Guest first_fit.cpp-style allocator. Tensor pointers remain fixed",
        "    # for the gem5 ABI; metadata and control flow are guest operations,",
        "    # while the heap payload is represented by shadow headers.",
        "ff_malloc_shadow:",
        f"    li      t0, 0x{allocator_base + ALLOCATOR_STATE_OFFSET:08x}",
        "    lw      t6, 4(t0)",
        "    beqz    t6, ff_malloc_fail",
        "    beqz    a0, ff_malloc_fail",
        "    addi    t2, a0, 15",
        "    srli    t2, t2, 4",
        "    addi    t2, t2, 1",
        "    li      t3, 1000",
        "    lw      t4, 0(t0)",
        f"    li      t0, 0x{allocator_base:08x}",
        "    lw      t1, 0(t4)",
        "ff_malloc_search:",
        "    addi    t3, t3, -1",
        "    beqz    t3, ff_malloc_fail",
        f"    li      t0, 0x{allocator_base + ALLOCATOR_STATE_OFFSET:08x}",
        f"    li      t6, 0x{allocator_base:08x}",
        "    beq     t1, t6, ff_malloc_header_valid",
        "    lw      t6, 8(t0)",
        "    bltu    t1, t6, ff_malloc_fail",
        "    lw      t6, 12(t0)",
        "    bgeu    t1, t6, ff_malloc_fail",
        "ff_malloc_header_valid:",
        f"    lw      t6, 8(t1)",
        f"    li      t0, 0x{ALLOCATOR_CANARY:08x}",
        "    bne     t6, t0, ff_malloc_fail",
        "    lw      t5, 0(t1)",
        f"    li      t0, 0x{allocator_base:08x}",
        "    beq     t5, t0, ff_malloc_valid_next",
        f"    li      t0, 0x{allocator_base + ALLOCATOR_STATE_OFFSET:08x}",
        "    lw      t6, 8(t0)",
        "    bltu    t5, t6, ff_malloc_fail",
        "    lw      t6, 12(t0)",
        "    bgeu    t5, t6, ff_malloc_fail",
        "ff_malloc_valid_next:",
        "    lw      t6, 4(t1)",
        "    bltu    t6, t2, ff_malloc_no_fit",
        "    beq     t6, t2, ff_malloc_exact_fit",
        "",
        "    # Split a free node and emulate p += p->meta.len.",
        "    sub     t6, t6, t2",
        "    sw      t6, 4(t1)",
        "    slli    t5, t6, 4",
        "    add     t5, t1, t5",
        "    jal     zero, ff_malloc_record",
        "",
        "ff_malloc_exact_fit:",
        "    lw      t5, 0(t1)",
        "    sw      t5, 0(t4)",
        f"    li      t0, 0x{ALLOCATOR_CANARY:08x}",
        "    sw      t0, 8(t1)",
        "    jal     zero, ff_malloc_record",
        "",
        "ff_malloc_no_fit:",
        f"    li      t0, 0x{allocator_base + ALLOCATOR_STATE_OFFSET:08x}",
        "    lw      t6, 0(t0)",
        "    beq     t1, t6, ff_malloc_out_of_memory",
        "    addi    t4, t1, 0",
        "    addi    t1, t5, 0",
        "    jal     zero, ff_malloc_search",
        "",
        "ff_malloc_record:",
        "    sw      zero, 0(a2)",
        "    sw      t2, 4(a2)",
        f"    li      t0, 0x{ALLOCATOR_CANARY:08x}",
        "    sw      t0, 8(a2)",
        "    sw      a1, 12(a2)",
        f"    li      t0, 0x{allocator_base + ALLOCATOR_STATE_OFFSET:08x}",
        "    sw      t4, 0(t0)",
        "    lw      t6, 20(t0)",
        "    addi    t6, t6, 1",
        "    sw      t6, 20(t0)",
        "    addi    a0, a1, 0",
        "    jalr    zero, 0(ra)",
        "",
        "    # Match ff_malloc's OOM accounting path for malformed/undersized heaps.",
        "ff_malloc_out_of_memory:",
        f"    li      t0, 0x{allocator_base + ALLOCATOR_STATE_OFFSET:08x}",
        "    lw      t1, 0(t0)",
        "    addi    t4, zero, 0",
        "    addi    t5, zero, 0",
        "    lw      t6, 0(t1)",
        "    li      t3, 1000",
        "ff_malloc_oom_scan:",
        "    addi    t3, t3, -1",
        "    beqz    t3, ff_malloc_oom_record",
        "    beq     t6, t1, ff_malloc_oom_record",
        "    beqz    t6, ff_malloc_oom_record",
        "    lw      t0, 4(t6)",
        "    li      t2, 1",
        "    bltu    t0, t2, ff_malloc_oom_next",
        "    addi    t0, t0, -1",
        "    slli    t0, t0, 4",
        "    add     t5, t5, t0",
        "    bgeu    t4, t0, ff_malloc_oom_next",
        "    addi    t4, t0, 0",
        "ff_malloc_oom_next:",
        "    lw      t6, 0(t6)",
        "    jal     zero, ff_malloc_oom_scan",
        "ff_malloc_oom_record:",
        f"    li      t0, 0x{allocator_base + ALLOCATOR_STATE_OFFSET:08x}",
        "    sw      t4, 24(t0)",
        "    sw      t5, 28(t0)",
        "ff_malloc_fail:",
        "    li      a0, 0",
        "    jalr    zero, 0(ra)",
    ])


def render_assembly(
    args: argparse.Namespace, config: dict[str, int]
) -> str:
    payload = payloads(args, config)
    stack = args.data_base + config["real_capacity"] - 4
    lines = [
        ".option norvc",
        ".section .text",
        ".globl _start",
        ".type _start,@function",
        "",
        "_start:",
        f"    li      sp, 0x{stack:08x}",
        f"    li      s0, 0x{args.input_base:08x}",
        f"    li      s1, 0x{args.weight_base:08x}",
        f"    li      s2, 0x{args.bias_base:08x}",
        f"    li      s3, 0x{args.output_base:08x}",
    ]
    if args.startup_model == TOOLCHAIN_APPROX_STARTUP:
        append_toolchain_approx_allocator(lines, args, config)
    if args.input_init == "cpu":
        if args.startup_model == TOOLCHAIN_APPROX_STARTUP:
            source_base = config["input_init_source_base"]
            if source_base is None:
                raise ValueError("input CPU initialization source is not available")
            lines.extend([
                "",
                "    # Toolchain-style A initialization: load source then store A.",
                "    li      t0, 0",
                f"    li      t1, {config['input_bytes']}",
                f"    li      t2, 0x{source_base:08x}",
                "input_init:",
                "    add     t3, t2, t0",
                "    lbu     t4, 0(t3)",
                "    add     t3, s0, t0",
                "    sb      t4, 0(t3)",
                "    addi    t0, t0, 1",
                "    bne     t0, t1, input_init",
            ])
        else:
            lines.extend([
                "",
                "    # Deterministic A initialization used by the independent verifier.",
                "    li      t0, 0",
                f"    li      t1, {config['input_bytes']}",
                "input_init:",
                "    srli    t2, t0, 4",
                "    add     t3, s0, t0",
                "    sb      t2, 0(t3)",
                "    addi    t0, t0, 1",
                "    bne     t0, t1, input_init",
            ])
    else:
        lines.extend([
            "",
            "    # A is preloaded in memory.hex; no CPU input initialization loop.",
        ])
    if args.weight_init == "cpu":
        source_base = config["weight_init_source_base"]
        if source_base is None:
            raise ValueError("weight CPU initialization source is not available")
        lines.extend([
            "",
            "    # Copy preloaded weight data into B with guest CPU byte stores.",
            "    li      t0, 0",
            f"    li      t1, {config['weight_bytes']}",
            f"    li      t2, 0x{source_base:08x}",
            "weight_init:",
            "    add     t3, t2, t0",
            "    lbu     t4, 0(t3)",
            "    add     t3, s1, t0",
            "    sb      t4, 0(t3)",
            "    addi    t0, t0, 1",
            "    bne     t0, t1, weight_init",
        ])
    else:
        lines.extend([
            "",
            "    # B is preloaded in memory.hex; no CPU weight initialization loop.",
        ])
    if args.bias_init == "cpu":
        source_base = config["bias_init_source_base"]
        if source_base is None:
            raise ValueError("bias CPU initialization source is not available")
        lines.extend([
            "",
            "    # Copy preloaded bias data into C with guest CPU halfword stores.",
            "    li      t0, 0",
            f"    li      t1, {args.out_channels}",
            f"    li      t2, 0x{source_base:08x}",
            "bias_init:",
            "    slli    t3, t0, 1",
            "    add     t4, t2, t3",
            "    lh      t5, 0(t4)",
            "    add     t4, s2, t3",
            "    sh      t5, 0(t4)",
            "    addi    t0, t0, 1",
            "    bne     t0, t1, bias_init",
        ])
    else:
        lines.extend([
            "",
            "    # C is preloaded in memory.hex; no CPU bias initialization loop.",
        ])
    if args.startup_model == TOOLCHAIN_APPROX_STARTUP:
        lines.extend([
            "",
            "    # Toolchain-style pre-submit mgetins4lsb idle poll.",
            "mgetins4_before:",
            f"    .word   0x{MGETINS4_LSB_WORD:08x}",
            "    bltz    t0, mgetins4_before",
        ])
    lines.extend(["", f"    .org    0x{CONFIG_START_PC:04x}"])
    for slot, value in enumerate(payload, start=1):
        lines.extend([
            f"    # msetins{slot}: 0x{value:016x}",
            f"    li      t0, 0x{low32(value):08x}",
            f"    li      t1, 0x{high32(value):08x}",
            f"    .word   0x{MSET_WORDS[slot]:08x}",
        ])
    lines.extend([
        "",
    ])
    if args.startup_model == TOOLCHAIN_APPROX_STARTUP:
        lines.extend([
            "    # Toolchain-style post-submit mgetins4lsb idle poll.",
            "mgetins4_after:",
            f"    .word   0x{MGETINS4_LSB_WORD:08x}",
            "    bltz    t0, mgetins4_after",
            "",
        ])
    lines.extend([
        "    jal     zero, shared_tail",
        f"    .org    0x{INSTRUCTION_TAIL_PC:04x}",
        "shared_tail:",
        "    ebreak",
    ])
    if args.startup_model == TOOLCHAIN_APPROX_STARTUP:
        append_toolchain_approx_allocator_helper(lines, config)
    lines.extend([
        ".size _start, . - _start",
        "",
    ])
    return "\n".join(lines)


def assemble(assembly: Path, output: Path, llvm_mc: str, llvm_objcopy: str) -> None:
    with tempfile.TemporaryDirectory(prefix="sau-n-custom-") as temp_name:
        temp = Path(temp_name)
        object_file = temp / "program.o"
        binary_file = temp / "program.bin"
        subprocess.run([
            llvm_mc,
            "--triple=riscv32-unknown-elf",
            "--filetype=obj",
            "-I",
            str(assembly.parent),
            str(assembly),
            "-o",
            str(object_file),
        ], check=True)
        subprocess.run([
            llvm_objcopy,
            "-O",
            "binary",
            "--only-section=.text",
            str(object_file),
            str(binary_file),
        ], check=True)
        binary = binary_file.read_bytes()
    if len(binary) > IMAGE_BYTES:
        raise ValueError(
            f"assembled program is {len(binary)} bytes; image limit is {IMAGE_BYTES}"
        )
    padded = binary + bytes(IMAGE_BYTES - len(binary))
    words = [
        int.from_bytes(padded[offset:offset + WORD_BYTES], "little")
        for offset in range(0, IMAGE_BYTES, WORD_BYTES)
    ]
    output.write_text(
        "\n".join(f"{word:08x}" for word in words) + "\n",
        encoding="ascii",
    )


def pattern_value(
    index: int, pattern: str, rng: random.Random, signed_limit: int
) -> int:
    if pattern == "zero":
        return 0
    if pattern == "ones":
        return 1
    if pattern == "ramp":
        return (index % 7) - 3
    return rng.randint(-signed_limit, signed_limit)


def write_memory(
    args: argparse.Namespace, config: dict[str, int], output: Path
) -> None:
    image = bytearray(args.data_size)
    data_base = args.data_base
    weight_rng = random.Random(args.seed)
    bias_rng = random.Random(args.seed + 1)

    def write_region(address: int, data: bytes, label: str) -> None:
        offset = address - data_base
        if offset < 0 or offset + len(data) > len(image):
            raise ValueError(f"{label} is outside generated data image")
        image[offset:offset + len(data)] = data

    weights = bytes(
        pattern_value(index, args.weight_pattern, weight_rng, 8) & 0xFF
        for index in range(config["weight_bytes"])
    )
    bias_values = [
        pattern_value(index, args.bias_pattern, bias_rng, 32)
        for index in range(args.out_channels)
    ]
    bias = b"".join(
        int(value).to_bytes(2, "little", signed=True)
        for value in bias_values
    )
    input_data = bytes(
        (index // 16) & 0xFF
        for index in range(config["input_bytes"])
    )
    if args.input_init == "memory":
        write_region(args.input_base, input_data, "input tensor")
    elif args.startup_model == TOOLCHAIN_APPROX_STARTUP:
        source_base = config["input_init_source_base"]
        if source_base is None:
            raise ValueError("input CPU initialization source is not available")
        write_region(source_base, input_data, "input initialization source")
    if args.weight_init == "memory":
        write_region(args.weight_base, weights, "weight tensor")
    else:
        source_base = config["weight_init_source_base"]
        if source_base is None:
            raise ValueError("weight CPU initialization source is not available")
        write_region(source_base, weights, "weight initialization source")
    if args.bias_init == "memory":
        write_region(args.bias_base, bias, "bias tensor")
    else:
        source_base = config["bias_init_source_base"]
        if source_base is None:
            raise ValueError("bias CPU initialization source is not available")
        write_region(source_base, bias, "bias initialization source")

    output.write_text(
        "\n".join(
            f"{int.from_bytes(image[offset:offset + WORD_BYTES], 'little'):08x}"
            for offset in range(0, len(image), WORD_BYTES)
        )
        + "\n",
        encoding="ascii",
    )


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_manifest(
    args: argparse.Namespace,
    config: dict[str, int],
    fixture_dir: Path,
    instruction_file: Path,
    memory_file: Path,
) -> None:
    input_init_region = (
        [0x0020, CONFIG_START_PC]
        if args.input_init == "cpu"
        else [0x0020, 0x0020]
    )
    modes = {
        "input": args.input_init,
        "weight": args.weight_init,
        "bias": args.bias_init,
    }
    cpu_written_tensors = [name for name, mode in modes.items() if mode == "cpu"]
    memory_preloaded_tensors = [
        name for name, mode in modes.items() if mode == "memory"
    ]
    source_regions = {}
    if (
        args.input_init == "cpu"
        and args.startup_model == TOOLCHAIN_APPROX_STARTUP
    ):
        source_regions["input"] = {
            "source_base": config["input_init_source_base"],
            "bytes": config["input_bytes"],
        }
    if args.weight_init == "cpu":
        source_regions["weight"] = {
            "source_base": config["weight_init_source_base"],
            "bytes": config["weight_bytes"],
        }
    if args.bias_init == "cpu":
        source_regions["bias"] = {
            "source_base": config["bias_init_source_base"],
            "bytes": config["bias_bytes"],
        }
    manifest = {
        "schema": "gem5-sau-n-custom-v1",
        "provenance": "generated by test_by_agent/conv3_e2e/run_custom_conv.py",
        "fixture": {
            "name": args.name,
            "mode": "four_ins_custom",
            "purpose": "custom sau_n full-offload e2e",
            "software_padding": False,
            "sau_instruction_counts": {
                "msetins1": 1,
                "msetins2": 1,
                "msetins3": 1,
                "msetins4": 1,
            },
        },
        "initialization": {
            "modes": modes,
            "cpu_written_tensors": cpu_written_tensors,
            "memory_preloaded_tensors": memory_preloaded_tensors,
            "source_regions": source_regions,
            "source_bytes": config["init_source_bytes"],
        },
        "startup": {
            "model": args.startup_model,
            "allocator": (
                {
                    "algorithm": "first-fit-shadow-v2",
                    "source": "kuiloong-NN/acenn/stdlib/first_fit.cpp",
                    "base": config["allocator_base"],
                    "bytes": config["allocator_bytes"],
                    "virtual_heap_headers": config["allocator_virtual_headers"],
                    "header_bytes": ALLOCATOR_ALIGNMENT,
                    "canary": f"0x{ALLOCATOR_CANARY:08x}",
                    "return_semantics": "fixed tensor address; shadow heap metadata only",
                    "operations": [
                        "init_heap bounds/alignment check",
                        "sentinel circular free-list setup",
                        "malloc size alignment and header-unit conversion",
                        "free-list header/canary/range validation",
                        "first-fit search",
                        "exact-fit unlink or split",
                        "OOM free-payload accounting",
                    ],
                    "malloc_initial": True,
                    "malloc_calls": ["input", "weight", "bias", "output"],
                    "malloc_call_details": [
                        {
                            "name": name,
                            "requested_bytes": size,
                            "true_size_headers": allocator_true_size(size),
                            "record_offset": record_offset,
                            "fixed_return": target,
                        }
                        for name, size, target, record_offset in (
                            (
                                "input",
                                config["input_bytes"],
                                args.input_base,
                                ALLOCATOR_RECORD_BASE,
                            ),
                            (
                                "weight",
                                config["weight_bytes"],
                                args.weight_base,
                                ALLOCATOR_RECORD_BASE + ALLOCATOR_RECORD_STRIDE,
                            ),
                            (
                                "bias",
                                config["bias_bytes"],
                                args.bias_base,
                                ALLOCATOR_RECORD_BASE + 2 * ALLOCATOR_RECORD_STRIDE,
                            ),
                            (
                                "output",
                                config["output_bytes"],
                                args.output_base,
                                ALLOCATOR_RECORD_BASE + 3 * ALLOCATOR_RECORD_STRIDE,
                            ),
                        )
                    ],
                }
                if args.startup_model == TOOLCHAIN_APPROX_STARTUP
                else None
            ),
            "input_source_load": (
                args.input_init == "cpu"
                and args.startup_model == TOOLCHAIN_APPROX_STARTUP
            ),
            "mgetins4_lsb_polls": (
                {"before_submit": 1, "after_submit": 1}
                if args.startup_model == TOOLCHAIN_APPROX_STARTUP
                else {"before_submit": 0, "after_submit": 0}
            ),
            "msetins4_wait": "gem5 blocking completion",
        },
        "image": {
            "instruction_base": 0,
            "instruction_bytes": IMAGE_BYTES,
            "instruction_words": IMAGE_BYTES // WORD_BYTES,
            "data_base": args.data_base,
            "data_window_bytes": args.data_size,
            "real_data_bytes": config["real_capacity"],
            "data_bank_size": args.data_bank_size,
            "data_bank_count": args.data_bank_count,
            "data_real_bank_count": args.data_real_bank_count,
            "format": "one 8-hex-digit 32-bit readmemh token per line",
            "word_to_cpu_bytes": "token value encoded little-endian",
        },
        "conv": {
            "N": config["n"],
            "C": config["c"],
            "H": config["h"],
            "W": config["w"],
            "OC": config["oc"],
            "kernel": 3,
            "stride": config["stride"],
            "padding": config["padding"],
            "cutbit": config["cutbit"],
            "input_base": args.input_base,
            "weight_base": args.weight_base,
            "bias_base": args.bias_base,
            "output_base": args.output_base,
            "input_bytes": config["input_bytes"],
            "weight_bytes": config["weight_bytes"],
            "bias_bytes": config["bias_bytes"],
            "output_bytes": config["output_bytes"],
            "input_initialization_mode": args.input_init,
            "input_initialization": (
                "CPU loads source and stores signed INT8 input[i] = i / 16"
                if (
                    args.input_init == "cpu"
                    and args.startup_model == TOOLCHAIN_APPROX_STARTUP
                )
                else "CPU stores signed INT8 input[i] = i / 16 bytewise"
                if args.input_init == "cpu"
                else "memory.hex preloads signed INT8 input[i] = i / 16"
            ),
            "weight_initialization_mode": args.weight_init,
            "weight_initialization": (
                "CPU copies preloaded source bytes into B"
                if args.weight_init == "cpu"
                else "memory.hex preloads B"
            ),
            "bias_initialization_mode": args.bias_init,
            "bias_initialization": (
                "CPU copies preloaded source halfwords into C"
                if args.bias_init == "cpu"
                else "memory.hex preloads C"
            ),
            "weight_pattern": args.weight_pattern,
            "bias_pattern": args.bias_pattern,
            "pattern_seed": args.seed,
        },
        "program_layout": {
            "startup_region": [0x0000, 0x0020],
            "input_init_region": input_init_region,
            "input_init_region_semantics": (
                "pre-config CPU startup region; includes allocator and other "
                "tensor initialization in toolchain-approx mode"
                if args.startup_model == TOOLCHAIN_APPROX_STARTUP
                else "input initialization loop"
            ),
            "tensor_initialization_region": (
                [0x0020, CONFIG_START_PC]
                if cpu_written_tensors
                else [0x0020, 0x0020]
            ),
            "pre_config_region": [0x0020, CONFIG_START_PC],
            "pre_config_region_semantics": (
                "tensor initialization code when selected, followed by zero-fill "
                "program layout before the CSR configuration region"
            ),
            "config_region": [CONFIG_START_PC, INSTRUCTION_TAIL_PC],
            "shared_tail_pc": INSTRUCTION_TAIL_PC,
            "termination": "ebreak at shared_tail_pc",
        },
        "payloads": [f"0x{value:016x}" for value in payloads(args, config)],
        "outputs": {
            "instruction_hex": {
                "path": instruction_file.name,
                "bytes": instruction_file.stat().st_size,
                "sha256": sha256(instruction_file),
            },
            "memory_hex": {
                "path": memory_file.name,
                "bytes": memory_file.stat().st_size,
                "sha256": sha256(memory_file),
            },
        },
    }
    (fixture_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def run_gem5(
    args: argparse.Namespace,
    config: dict[str, int],
    fixture_dir: Path,
) -> Path:
    gem5 = args.gem5
    if not gem5.is_file() or not gem5.stat().st_mode & 0o111:
        raise FileNotFoundError(
            f"gem5 executable is missing or not executable: {gem5}"
        )
    if not args.config.is_file():
        raise FileNotFoundError(f"gem5 config is missing: {args.config}")

    run_dir = fixture_dir / "run"
    run_dir.mkdir(parents=True, exist_ok=True)
    trace_file = run_dir / "cycle_trace.log"
    stdout_file = run_dir / "stdout.log"
    stderr_file = run_dir / "stderr.log"
    command = [
        str(gem5),
        "-d",
        str(run_dir),
        str(args.config),
        "--mem-system",
        "rtl-dut-kui-tb",
        "--clock-frequency",
        args.clock_frequency,
        "--reset-cycles",
        str(args.reset_cycles),
        "--max-cycles",
        str(args.max_cycles),
        "--no-icache",
        "--cycle-trace-compact",
        "--terminate-on-ebreak",
        "--veu-model",
        "fake",
        "--sau-model",
        "sau_n",
        "--rtl-data-base",
        f"0x{args.data_base:x}",
        "--rtl-data-size",
        f"0x{args.data_size:x}",
        "--rtl-data-bank-size",
        f"0x{args.data_bank_size:x}",
        "--rtl-data-bank-count",
        str(args.data_bank_count),
        "--rtl-data-real-bank-count",
        str(args.data_real_bank_count),
        "--program-file",
        str(fixture_dir / "instruction.hex"),
        "--dmem-hex",
        str(fixture_dir / "memory.hex"),
        "--cycle-trace",
        str(trace_file),
    ]
    print("running:", " ".join(command))
    with stdout_file.open("w", encoding="utf-8") as stdout, \
            stderr_file.open("w", encoding="utf-8") as stderr:
        subprocess.run(command, check=True, stdout=stdout, stderr=stderr)

    report_file = run_dir / "report.json"
    verifier = SCRIPT_DIR / "verify_conv3_e2e.py"
    subprocess.run([
        sys.executable,
        str(verifier),
        "--fixture-dir",
        str(fixture_dir),
        "--instruction-hex",
        str(fixture_dir / "instruction.hex"),
        "--stats",
        str(run_dir / "stats.txt"),
        "--cycle-trace",
        str(trace_file),
        "--sau-model",
        "sau_n",
        "--json-report",
        str(report_file),
    ], check=True)
    return report_file


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate and run one configurable sau_n Conv2d case."
    )
    parser.add_argument("--name", default="custom_conv")
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--gem5", type=Path, default=REPO_ROOT / "build/RISCV/gem5.opt")
    parser.add_argument("--config", type=Path, default=REPO_ROOT / "configs/brs/run_pipeline_mini.py")
    parser.add_argument("--llvm-mc", default="/usr/bin/llvm-mc")
    parser.add_argument("--llvm-objcopy", default="/usr/bin/llvm-objcopy")

    parser.add_argument("--n", type=int, default=1)
    parser.add_argument("--channels", type=int, default=16)
    parser.add_argument("--height", type=int, default=16)
    parser.add_argument("--width", type=int, default=32)
    parser.add_argument("--out-channels", type=int, default=16)
    parser.add_argument("--padding", type=int, default=1)
    parser.add_argument("--stride", type=int, default=1)
    parser.add_argument("--cutbit", type=int, default=12)

    parser.add_argument("--input-base", type=parse_int, default=0x29130000)
    parser.add_argument("--weight-base", type=parse_int, default=0x29132000)
    parser.add_argument("--bias-base", type=parse_int, default=0x29132900)
    parser.add_argument("--output-base", type=parse_int, default=0x29132920)
    parser.add_argument("--data-base", type=parse_int, default=DEFAULT_DATA_BASE)
    parser.add_argument("--data-size", type=parse_int, default=DEFAULT_DATA_SIZE)
    parser.add_argument("--data-bank-size", type=parse_int, default=DEFAULT_BANK_SIZE)
    parser.add_argument("--data-bank-count", type=int, default=DEFAULT_BANK_COUNT)
    parser.add_argument("--data-real-bank-count", type=int, default=DEFAULT_REAL_BANK_COUNT)

    parser.add_argument(
        "--input-init",
        choices=("cpu", "memory"),
        default="cpu",
        help="Initialize A with guest CPU stores or preload A in memory.hex.",
    )
    parser.add_argument(
        "--weight-init",
        choices=("cpu", "memory"),
        default="cpu",
        help="Initialize B with guest CPU stores or preload B in memory.hex.",
    )
    parser.add_argument(
        "--bias-init",
        choices=("cpu", "memory"),
        default="cpu",
        help="Initialize C with guest CPU stores or preload C in memory.hex.",
    )
    parser.add_argument(
        "--tensor-init",
        choices=("cpu", "memory"),
        default=None,
        help="Set input, weight and bias initialization mode together.",
    )
    parser.add_argument(
        "--startup-model",
        choices=(MINIMAL_STARTUP, TOOLCHAIN_APPROX_STARTUP),
        default=TOOLCHAIN_APPROX_STARTUP,
        help=(
            "Use the toolchain-style approximate allocator/source-load/poll "
            "startup, or the legacy minimal runner startup."
        ),
    )

    patterns = ("zero", "ones", "ramp", "random")
    parser.add_argument("--weight-pattern", choices=patterns, default="ramp")
    parser.add_argument("--bias-pattern", choices=patterns, default="zero")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--max-cycles", type=int, default=2_000_000)
    parser.add_argument("--reset-cycles", type=int, default=10)
    parser.add_argument("--clock-frequency", default="100MHz")
    parser.add_argument(
        "--no-run",
        action="store_true",
        help="Only generate the fixture; do not invoke gem5 or the verifier.",
    )
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    args.output_root = args.output_root.resolve()
    args.gem5 = args.gem5.resolve()
    args.config = args.config.resolve()
    if args.tensor_init is not None:
        args.input_init = args.tensor_init
        args.weight_init = args.tensor_init
        args.bias_init = args.tensor_init
    try:
        config = validate_and_derive(args)
        fixture_dir = args.output_root / args.name
        fixture_dir.mkdir(parents=True, exist_ok=True)
        assembly_file = fixture_dir / "program.S"
        instruction_file = fixture_dir / "instruction.hex"
        memory_file = fixture_dir / "memory.hex"
        assembly_file.write_text(
            render_assembly(args, config),
            encoding="utf-8",
        )
        assemble(assembly_file, instruction_file, args.llvm_mc, args.llvm_objcopy)
        write_memory(args, config, memory_file)
        write_manifest(args, config, fixture_dir, instruction_file, memory_file)

        print(f"generated fixture: {fixture_dir}")
        print("payloads:")
        for index, value in enumerate(payloads(args, config), start=1):
            print(f"  msetins{index}=0x{value:016x}")
        print(
            "shape: "
            f"N={config['n']} C={config['c']} H={config['h']} W={config['w']} "
            f"OC={config['oc']} -> out={config['out_h']}x{config['out_w']}"
        )
        print(
            "scratchpad rows: "
            f"A={config['a_rows']} B={config['b_rows']} "
            f"C=2 D={config['d_rows']} total={config['scratchpad_rows']}"
        )
        print(
            "tensor initialization: "
            f"input={args.input_init} weight={args.weight_init} bias={args.bias_init}"
        )
        if args.no_run:
            return 0
        report = run_gem5(args, config, fixture_dir)
        print(f"PASS: report written to {report}")
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"custom sau_n run failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
