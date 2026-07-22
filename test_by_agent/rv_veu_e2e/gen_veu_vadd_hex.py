#!/usr/bin/env python3
"""Generate a minimal RV-to-VEU VADD end-to-end test image."""

from pathlib import Path
import struct


ROOT = Path(__file__).resolve().parent

INSTR_HEX = ROOT / "instr_mem.hex"
DATA_HEX = ROOT / "data_mem.hex"

VEUCFG = 0x104
VEUSTATUS = 0x100
VEUWADDR = 0x103
VEUVLEN = 0x105
VEUMASK = 0x106


def encode_addi(rd, rs1, imm):
    return ((imm & 0xFFF) << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | 0x13


def encode_lw(rd, rs1, imm):
    return ((imm & 0xFFF) << 20) | (rs1 << 15) | (0x2 << 12) | (rd << 7) | 0x03


def encode_andi(rd, rs1, imm):
    return ((imm & 0xFFF) << 20) | (rs1 << 15) | (0x7 << 12) | (rd << 7) | 0x13


def encode_bne(rs1, rs2, imm):
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


def encode_vsetcsr(rs1, csr):
    return (csr << 20) | (rs1 << 15) | (0x2 << 12) | 0x0B


def encode_vgetcsr(rd, csr):
    return (csr << 20) | (0x3 << 12) | (rd << 7) | 0x0B


def encode_vadd(rd, rs1, rs2):
    return (rs2 << 20) | (rs1 << 15) | (rd << 7) | 0x6B


def write_word_hex(path, words):
    with path.open("w", encoding="ascii") as out:
        for word in words:
            out.write(f"{word:08x}\n")


def write_byte_hex(path, data):
    with path.open("w", encoding="ascii") as out:
        for offset in range(0, len(data), 16):
            row = data[offset:offset + 16]
            out.write(" ".join(f"{byte:02x}" for byte in row))
            out.write("\n")


def put_u32_lanes(data, base, lanes):
    for index, value in enumerate(lanes):
        offset = base + index * 4
        data[offset:offset + 4] = struct.pack("<I", value)


def main():
    setup = [
        encode_addi(1, 0, 0x100),       # x1 = VEU raddr1
        encode_addi(2, 0, 0x200),       # x2 = VEU raddr2
        encode_addi(3, 0, 0x300),       # x3 = VEU waddr
        encode_addi(5, 0, 0x100),       # x5 = VEUVLEN, 256 bits
        encode_addi(6, 0, -1),          # x6 = VEUMASK, all bytes enabled
        encode_vsetcsr(0, VEUCFG),
        encode_vsetcsr(3, VEUWADDR),
        encode_vsetcsr(5, VEUVLEN),
        encode_vsetcsr(6, VEUMASK),
        encode_vadd(7, 1, 2),
    ]
    polling_program = setup + [
        encode_vgetcsr(10, VEUSTATUS),  # poll_loop: read VEU busy status
        encode_andi(10, 10, 1),         # keep busy bit
        encode_bne(10, 0, -8),          # busy -> poll_loop
        encode_lw(8, 0, 0x300),         # read lane 0, expected 11
        encode_lw(9, 0, 0x31C),         # read lane 7, expected 88
        0x00100073,                     # ebreak
    ]
    data = bytearray(0x320)
    put_u32_lanes(data, 0x100, [1, 2, 3, 4, 5, 6, 7, 8])
    put_u32_lanes(data, 0x200, [10, 20, 30, 40, 50, 60, 70, 80])

    write_word_hex(INSTR_HEX, polling_program)
    write_byte_hex(DATA_HEX, data)

    print(f"wrote {INSTR_HEX}")
    print(f"wrote {DATA_HEX}")


if __name__ == "__main__":
    main()
