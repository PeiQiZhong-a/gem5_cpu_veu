#!/usr/bin/env python3
"""Generate the RV32 driver and compressed input for Mikui DMA E2E."""

from pathlib import Path


ROOT = Path(__file__).resolve().parent


def lui(rd, imm20):
    return ((imm20 & 0xFFFFF) << 12) | (rd << 7) | 0x37


def addi(rd, rs1, imm):
    return ((imm & 0xFFF) << 20) | (rs1 << 15) | (rd << 7) | 0x13


def sw(rs2, rs1, imm):
    encoded = imm & 0xFFF
    return (
        ((encoded >> 5) & 0x7F) << 25
        | (rs2 << 20)
        | (rs1 << 15)
        | (0x2 << 12)
        | (encoded & 0x1F) << 7
        | 0x23
    )


def branch(rs1, rs2, imm, funct3):
    encoded = imm & 0x1FFF
    return (
        ((encoded >> 12) & 1) << 31
        | ((encoded >> 5) & 0x3F) << 25
        | (rs2 << 20)
        | (rs1 << 15)
        | (funct3 << 12)
        | ((encoded >> 1) & 0xF) << 8
        | ((encoded >> 11) & 1) << 7
        | 0x63
    )


def bne(rs1, rs2, imm):
    return branch(rs1, rs2, imm, 1)


def compressed_input():
    # Dense, k=2, one four-element block. Values decode to 0, -1, 1, -2.
    meta = (4 << 20) | (1 << 4) | (2 << 1)
    data = bytearray(meta.to_bytes(4, "big"))
    units = [0, 1, 2, 3]
    bits = []
    for unit in units:
        quotient = unit >> 2
        remainder = unit & 3
        bits.extend([1] * quotient)
        bits.append(0)
        bits.extend([(remainder >> bit) & 1 for bit in (1, 0)])
    bits.extend([0] * ((-len(bits)) % 32))
    payload = sum(bit << (31 - index) for index, bit in enumerate(bits))
    data.extend(payload.to_bytes(4, "little"))
    return bytes(data)


def main():
    # Program the actual dt_dma four-register ABI, then leave enough guest
    # cycles for the independently scheduled 32-bit AHB reads and writes.
    words = [
        lui(1, 0x60000),       # x1 = compressed source SRAM
        lui(2, 0x60001),       # x2 = decompressed destination SRAM
        lui(4, 0x4001A),
        addi(4, 4, -0x400),    # x4 = 0x40019c00 register base
        sw(1, 4, 0x04),        # SRC
        sw(2, 4, 0x08),        # DST
        addi(5, 0, 8),
        sw(5, 4, 0x0C),        # LENGTH in bytes
        addi(5, 0, 1),
        sw(5, 4, 0x00),        # CTRL.START
        addi(6, 0, 200),
        addi(6, 6, -1),
        bne(6, 0, -4),
        0x00100073,            # EBREAK
    ]
    (ROOT / "instr_mem.hex").write_text(
        "".join(f"{word:08x}\n" for word in words), encoding="ascii")
    (ROOT / "dma_input.bin").write_bytes(compressed_input())
    print("generated RV32 driver and 8-byte compressed DMA input")


if __name__ == "__main__":
    main()
