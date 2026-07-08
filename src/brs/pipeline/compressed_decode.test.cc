#include "brs/pipeline/compressed_decode.hh"

#include <cstdint>

#include <gtest/gtest.h>

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
encodeI(uint32_t imm, uint32_t rs1, uint32_t funct3,
        uint32_t rd, uint32_t opcode)
{
    return ((imm & 0xfff) << 20) | (rs1 << 15) | (funct3 << 12) |
        (rd << 7) | opcode;
}

uint32_t
encodeR(uint32_t funct7, uint32_t rs2, uint32_t rs1,
        uint32_t funct3, uint32_t rd, uint32_t opcode)
{
    return (funct7 << 25) | (rs2 << 20) | (rs1 << 15) |
        (funct3 << 12) | (rd << 7) | opcode;
}

uint32_t
encodeS(uint32_t imm, uint32_t rs2, uint32_t rs1,
        uint32_t funct3, uint32_t opcode)
{
    return (((imm >> 5) & 0x7f) << 25) | (rs2 << 20) | (rs1 << 15) |
        (funct3 << 12) | ((imm & 0x1f) << 7) | opcode;
}

uint32_t
encodeB(uint32_t imm, uint32_t rs2, uint32_t rs1,
        uint32_t funct3, uint32_t opcode)
{
    return (((imm >> 12) & 0x1) << 31) |
        (((imm >> 5) & 0x3f) << 25) | (rs2 << 20) | (rs1 << 15) |
        (funct3 << 12) | (((imm >> 1) & 0xf) << 8) |
        (((imm >> 11) & 0x1) << 7) | opcode;
}

uint32_t
encodeU(uint32_t imm20, uint32_t rd, uint32_t opcode)
{
    return (imm20 << 12) | (rd << 7) | opcode;
}

uint32_t
encodeJ(uint32_t imm, uint32_t rd, uint32_t opcode)
{
    return (((imm >> 20) & 0x1) << 31) |
        (((imm >> 1) & 0x3ff) << 21) | (((imm >> 11) & 0x1) << 20) |
        (((imm >> 12) & 0xff) << 12) | (rd << 7) | opcode;
}

void
expectExpand(uint16_t compressed, uint32_t expected)
{
    uint32_t expanded = 0;
    EXPECT_TRUE(expandCompressedInstr(compressed, expanded));
    EXPECT_EQ(expanded, expected) << "compressed=0x" << std::hex
        << compressed;
}

uint16_t
cBase(uint32_t funct3, uint32_t quadrant)
{
    return static_cast<uint16_t>((funct3 << 13) | quadrant);
}

} // anonymous namespace

TEST(CompressedDecodeTest, PassesThroughRv32Instructions)
{
    uint32_t expanded = 0;
    EXPECT_FALSE(isCompressedInstr(0x00500093));
    EXPECT_TRUE(expandCompressedInstr(0x00500093, expanded));
    EXPECT_EQ(expanded, 0x00500093);
}

TEST(CompressedDecodeTest, ExpandsQuadrant0LoadsStoresAndAddi4spn)
{
    expectExpand(cBase(0, 0) | (1 << 6) | (1 << 2),
                 encodeI(4, 2, 0, 9, OPCODE_OPIMM));

    expectExpand(cBase(2, 0) | (2 << 7) | (3 << 2) | (1 << 6),
                 encodeI(4, 10, 2, 11, OPCODE_LOAD));

    expectExpand(cBase(6, 0) | (2 << 7) | (3 << 2) | (1 << 6),
                 encodeS(4, 11, 10, 2, OPCODE_STORE));
}

TEST(CompressedDecodeTest, ExpandsQuadrant1ImmediateAndJumpForms)
{
    expectExpand(cBase(0, 1) | (1 << 7) | (5 << 2),
                 encodeI(5, 1, 0, 1, OPCODE_OPIMM));
    expectExpand(cBase(1, 1) | (1 << 3), encodeJ(2, 1, OPCODE_JAL));
    expectExpand(cBase(5, 1) | (1 << 3), encodeJ(2, 0, OPCODE_JAL));
    expectExpand(cBase(2, 1) | (3 << 7) | (6 << 2),
                 encodeI(6, 0, 0, 3, OPCODE_OPIMM));
    expectExpand(cBase(3, 1) | (2 << 7) | (1 << 6),
                 encodeI(16, 2, 0, 2, OPCODE_OPIMM));
    expectExpand(cBase(3, 1) | (3 << 7) | (1 << 2),
                 encodeU(1, 3, OPCODE_LUI));
}

TEST(CompressedDecodeTest, ExpandsQuadrant1AluAndBranchForms)
{
    expectExpand(cBase(4, 1) | (1 << 7) | (3 << 2),
                 encodeI(3, 9, 5, 9, OPCODE_OPIMM));
    expectExpand(cBase(4, 1) | (1 << 10) | (1 << 7) | (3 << 2),
                 encodeI((0x20 << 5) | 3, 9, 5, 9, OPCODE_OPIMM));
    expectExpand(cBase(4, 1) | (2 << 10) | (1 << 7) | (3 << 2),
                 encodeI(3, 9, 7, 9, OPCODE_OPIMM));

    expectExpand(cBase(4, 1) | (3 << 10) | (1 << 7) | (2 << 2),
                 encodeR(0x20, 10, 9, 0, 9, OPCODE_OP));
    expectExpand(cBase(4, 1) | (3 << 10) | (1 << 7) | (1 << 5) | (2 << 2),
                 encodeR(0, 10, 9, 4, 9, OPCODE_OP));
    expectExpand(cBase(4, 1) | (3 << 10) | (1 << 7) | (2 << 5) | (2 << 2),
                 encodeR(0, 10, 9, 6, 9, OPCODE_OP));
    expectExpand(cBase(4, 1) | (3 << 10) | (1 << 7) | (3 << 5) | (2 << 2),
                 encodeR(0, 10, 9, 7, 9, OPCODE_OP));

    expectExpand(cBase(6, 1) | (1 << 7) | (1 << 3),
                 encodeB(2, 0, 9, 0, OPCODE_BRANCH));
    expectExpand(cBase(7, 1) | (1 << 7) | (1 << 3),
                 encodeB(2, 0, 9, 1, OPCODE_BRANCH));
}

TEST(CompressedDecodeTest, ExpandsQuadrant2StackAndRegisterForms)
{
    expectExpand(cBase(0, 2) | (4 << 7) | (2 << 2),
                 encodeI(2, 4, 1, 4, OPCODE_OPIMM));
    expectExpand(cBase(2, 2) | (5 << 7) | (1 << 4),
                 encodeI(4, 2, 2, 5, OPCODE_LOAD));
    expectExpand(cBase(4, 2) | (5 << 7),
                 encodeI(0, 5, 0, 0, OPCODE_JALR));
    expectExpand(cBase(4, 2) | (5 << 7) | (6 << 2),
                 encodeR(0, 6, 0, 0, 5, OPCODE_OP));
    expectExpand(0x9002, 0x00100073);
    expectExpand(cBase(4, 2) | (1 << 12) | (5 << 7),
                 encodeI(0, 5, 0, 1, OPCODE_JALR));
    expectExpand(cBase(4, 2) | (1 << 12) | (5 << 7) | (6 << 2),
                 encodeR(0, 6, 5, 0, 5, OPCODE_OP));
    expectExpand(cBase(6, 2) | (7 << 2) | (1 << 9),
                 encodeS(4, 7, 2, 2, OPCODE_STORE));
}

TEST(CompressedDecodeTest, RejectsUnsupportedCompressedPatterns)
{
    uint32_t expanded = 0;
    EXPECT_FALSE(expandCompressedInstr(cBase(1, 0), expanded));
}

} // namespace gem5
