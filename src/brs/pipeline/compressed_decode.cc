#include "brs/pipeline/compressed_decode.hh"

namespace gem5
{

namespace
{

constexpr uint32_t OPCODE_OPIMM = 0x13;
constexpr uint32_t OPCODE_OP = 0x33;
constexpr uint32_t OPCODE_LOAD = 0x03;
constexpr uint32_t OPCODE_STORE = 0x23;
constexpr uint32_t OPCODE_JAL = 0x6f;
constexpr uint32_t OPCODE_JALR = 0x67;
constexpr uint32_t OPCODE_LUI = 0x37;
constexpr uint32_t OPCODE_BRANCH = 0x63;

uint32_t
bits(uint32_t value, unsigned hi, unsigned lo)
{
    return (value >> lo) & ((1u << (hi - lo + 1)) - 1);
}

uint32_t
bit(uint32_t value, unsigned pos)
{
    return (value >> pos) & 1u;
}

uint32_t
regPrime(uint32_t reg3)
{
    return 8u + reg3;
}

int32_t
signExtend(uint32_t value, unsigned width)
{
    const uint32_t sign = 1u << (width - 1);
    return static_cast<int32_t>((value ^ sign) - sign);
}

uint32_t
encodeI(uint32_t imm, uint32_t rs1, uint32_t funct3,
        uint32_t rd, uint32_t opcode)
{
    return ((imm & 0xfff) << 20) | ((rs1 & 0x1f) << 15) |
        ((funct3 & 0x7) << 12) | ((rd & 0x1f) << 7) | (opcode & 0x7f);
}

uint32_t
encodeR(uint32_t funct7, uint32_t rs2, uint32_t rs1,
        uint32_t funct3, uint32_t rd, uint32_t opcode)
{
    return ((funct7 & 0x7f) << 25) | ((rs2 & 0x1f) << 20) |
        ((rs1 & 0x1f) << 15) | ((funct3 & 0x7) << 12) |
        ((rd & 0x1f) << 7) | (opcode & 0x7f);
}

uint32_t
encodeS(uint32_t imm, uint32_t rs2, uint32_t rs1,
        uint32_t funct3, uint32_t opcode)
{
    return (((imm >> 5) & 0x7f) << 25) | ((rs2 & 0x1f) << 20) |
        ((rs1 & 0x1f) << 15) | ((funct3 & 0x7) << 12) |
        ((imm & 0x1f) << 7) | (opcode & 0x7f);
}

uint32_t
encodeB(uint32_t imm, uint32_t rs2, uint32_t rs1,
        uint32_t funct3, uint32_t opcode)
{
    return (((imm >> 12) & 0x1) << 31) |
        (((imm >> 5) & 0x3f) << 25) | ((rs2 & 0x1f) << 20) |
        ((rs1 & 0x1f) << 15) | ((funct3 & 0x7) << 12) |
        (((imm >> 1) & 0xf) << 8) | (((imm >> 11) & 0x1) << 7) |
        (opcode & 0x7f);
}

uint32_t
encodeU(uint32_t imm20, uint32_t rd, uint32_t opcode)
{
    return ((imm20 & 0xfffff) << 12) | ((rd & 0x1f) << 7) |
        (opcode & 0x7f);
}

uint32_t
encodeJ(uint32_t imm, uint32_t rd, uint32_t opcode)
{
    return (((imm >> 20) & 0x1) << 31) |
        (((imm >> 1) & 0x3ff) << 21) | (((imm >> 11) & 0x1) << 20) |
        (((imm >> 12) & 0xff) << 12) | ((rd & 0x1f) << 7) |
        (opcode & 0x7f);
}

uint32_t
cjImm(uint32_t c)
{
    uint32_t imm = 0;
    imm |= bit(c, 12) << 11;
    imm |= bit(c, 11) << 4;
    imm |= bits(c, 10, 9) << 8;
    imm |= bit(c, 8) << 10;
    imm |= bit(c, 7) << 6;
    imm |= bit(c, 6) << 7;
    imm |= bits(c, 5, 3) << 1;
    imm |= bit(c, 2) << 5;
    return static_cast<uint32_t>(signExtend(imm, 12));
}

uint32_t
cbImm(uint32_t c)
{
    uint32_t imm = 0;
    imm |= bit(c, 12) << 8;
    imm |= bits(c, 6, 5) << 6;
    imm |= bit(c, 2) << 5;
    imm |= bits(c, 11, 10) << 3;
    imm |= bits(c, 4, 3) << 1;
    return static_cast<uint32_t>(signExtend(imm, 9));
}

} // anonymous namespace

bool
isCompressedInstr(uint32_t instr)
{
    return (instr & 0x3) != 0x3;
}

bool
expandCompressedInstr(uint32_t instr, uint32_t &expanded)
{
    if (!isCompressedInstr(instr)) {
        expanded = instr;
        return true;
    }

    const uint32_t c = instr & 0xffff;
    const uint32_t quadrant = c & 0x3;
    const uint32_t funct3 = bits(c, 15, 13);
    const uint32_t rd5 = bits(c, 11, 7);
    const uint32_t rs1_5 = rd5;
    const uint32_t rs2_5 = bits(c, 6, 2);
    const uint32_t rd3 = bits(c, 4, 2);
    const uint32_t rs1_3 = bits(c, 9, 7);
    const uint32_t rs2_3 = bits(c, 4, 2);

    if (quadrant == 0x0) {
        if (funct3 == 0x0) {
            const uint32_t imm = (bits(c, 10, 7) << 6) |
                (bits(c, 12, 11) << 4) | (bit(c, 5) << 3) |
                (bit(c, 6) << 2);
            expanded = encodeI(imm, 2, 0x0, regPrime(rd3), OPCODE_OPIMM);
            return true;
        }
        if (funct3 == 0x2) {
            const uint32_t imm = (bit(c, 5) << 6) |
                (bits(c, 12, 10) << 3) | (bit(c, 6) << 2);
            expanded = encodeI(imm, regPrime(rs1_3), 0x2,
                               regPrime(rd3), OPCODE_LOAD);
            return true;
        }
        if (funct3 == 0x6) {
            const uint32_t imm = (bit(c, 5) << 6) | (bit(c, 12) << 5) |
                (bits(c, 11, 10) << 3) | (bit(c, 6) << 2);
            expanded = encodeS(imm, regPrime(rs2_3), regPrime(rs1_3),
                               0x2, OPCODE_STORE);
            return true;
        }
    } else if (quadrant == 0x1) {
        if (funct3 == 0x0) {
            const uint32_t imm = static_cast<uint32_t>(
                signExtend((bit(c, 12) << 5) | bits(c, 6, 2), 6));
            expanded = encodeI(imm, rs1_5, 0x0, rd5, OPCODE_OPIMM);
            return true;
        }
        if (funct3 == 0x1 || funct3 == 0x5) {
            const uint32_t rd = (funct3 == 0x1) ? 1 : 0;
            expanded = encodeJ(cjImm(c), rd, OPCODE_JAL);
            return true;
        }
        if (funct3 == 0x2) {
            const uint32_t imm = static_cast<uint32_t>(
                signExtend((bit(c, 12) << 5) | bits(c, 6, 2), 6));
            expanded = encodeI(imm, 0, 0x0, rd5, OPCODE_OPIMM);
            return true;
        }
        if (funct3 == 0x3) {
            if (rd5 == 2) {
                const uint32_t imm = static_cast<uint32_t>(
                    signExtend((bit(c, 12) << 9) | (bits(c, 4, 3) << 7) |
                               (bit(c, 5) << 6) | (bit(c, 2) << 5) |
                               (bit(c, 6) << 4), 10));
                expanded = encodeI(imm, 2, 0x0, 2, OPCODE_OPIMM);
            } else {
                const uint32_t imm20 =
                    (bit(c, 12) ? 0xfffe0u : 0u) | bits(c, 6, 2);
                expanded = encodeU(imm20, rd5, OPCODE_LUI);
            }
            return true;
        }
        if (funct3 == 0x4) {
            const uint32_t op = bits(c, 11, 10);
            const uint32_t rd = regPrime(rs1_3);
            if (op == 0x0 || op == 0x1) {
                const uint32_t shamt = bits(c, 6, 2);
                const uint32_t funct7 = (op == 0x1) ? 0x20 : 0x00;
                expanded = encodeI((funct7 << 5) | shamt, rd, 0x5,
                                   rd, OPCODE_OPIMM);
                return true;
            }
            if (op == 0x2) {
                const uint32_t imm = static_cast<uint32_t>(
                    signExtend((bit(c, 12) << 5) | bits(c, 6, 2), 6));
                expanded = encodeI(imm, rd, 0x7, rd, OPCODE_OPIMM);
                return true;
            }
            if (op == 0x3 && bit(c, 12) == 0) {
                const uint32_t subop = bits(c, 6, 5);
                const uint32_t rs2 = regPrime(rs2_3);
                const uint32_t funct7 = (subop == 0x0) ? 0x20 : 0x00;
                const uint32_t funct3_r =
                    (subop == 0x0) ? 0x0 :
                    (subop == 0x1) ? 0x4 :
                    (subop == 0x2) ? 0x6 : 0x7;
                expanded = encodeR(funct7, rs2, rd, funct3_r, rd, OPCODE_OP);
                return true;
            }
        }
        if (funct3 == 0x6 || funct3 == 0x7) {
            const uint32_t rs1 = regPrime(rs1_3);
            const uint32_t funct3_b = (funct3 == 0x6) ? 0x0 : 0x1;
            expanded = encodeB(cbImm(c), 0, rs1, funct3_b, OPCODE_BRANCH);
            return true;
        }
    } else if (quadrant == 0x2) {
        if (funct3 == 0x0) {
            const uint32_t shamt = bits(c, 6, 2);
            expanded = encodeI(shamt, rd5, 0x1, rd5, OPCODE_OPIMM);
            return true;
        }
        if (funct3 == 0x2) {
            const uint32_t imm = (bits(c, 3, 2) << 6) |
                (bit(c, 12) << 5) | (bits(c, 6, 4) << 2);
            expanded = encodeI(imm, 2, 0x2, rd5, OPCODE_LOAD);
            return true;
        }
        if (funct3 == 0x4) {
            if (bit(c, 12) == 0) {
                if (rs2_5 == 0) {
                    expanded = encodeI(0, rs1_5, 0x0, 0, OPCODE_JALR);
                } else {
                    expanded = encodeR(0, rs2_5, 0, 0x0, rd5, OPCODE_OP);
                }
                return true;
            }
            if (rs2_5 == 0) {
                if (rs1_5 == 0) {
                    expanded = 0x00100073;
                } else {
                    expanded = encodeI(0, rs1_5, 0x0, 1, OPCODE_JALR);
                }
            } else {
                expanded = encodeR(0, rs2_5, rs1_5, 0x0, rd5, OPCODE_OP);
            }
            return true;
        }
        if (funct3 == 0x6) {
            const uint32_t imm = (bits(c, 8, 7) << 6) |
                (bit(c, 12) << 5) | (bits(c, 11, 9) << 2);
            expanded = encodeS(imm, rs2_5, 2, 0x2, OPCODE_STORE);
            return true;
        }
    }

    expanded = 0;
    return false;
}

} // namespace gem5
