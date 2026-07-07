#include "brs/pipeline/veu_decode.hh"

#include <array>

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

constexpr uint32_t
encodeCsr(uint16_t csr, uint8_t rs1, uint8_t function3, uint8_t rd)
{
    return (static_cast<uint32_t>(csr) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(function3) << 12) |
           (static_cast<uint32_t>(rd) << 7) |
           VeuCsrOpcode;
}

constexpr uint32_t
encodeVector(uint8_t function7, uint8_t rs2, uint8_t rs1, uint8_t rd)
{
    return (static_cast<uint32_t>(function7) << 25) |
           (static_cast<uint32_t>(rs2) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(rd) << 7) |
           VeuVectorOpcode;
}

constexpr uint32_t
encodeThreeSource(
    uint8_t rs3, uint8_t function3, uint8_t rs2, uint8_t rs1, uint8_t rd)
{
    return (static_cast<uint32_t>(rs3) << 27) |
           (uint32_t{1} << 25) |
           (static_cast<uint32_t>(rs2) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(function3) << 12) |
           (static_cast<uint32_t>(rd) << 7) |
           VeuThreeSourceOpcode;
}

TEST(SpiritVeuDecodeTest, DecodesAllFourCsrInstructions)
{
    struct Case
    {
        uint8_t function3;
        VeuInstruction operation;
        VeuWriteType writeType;
        bool csrWrite;
        bool writesRd;
    };

    constexpr std::array<Case, 4> cases{{
        {0, VeuInstruction::CsrOr, VeuWriteType::Set, true, true},
        {1, VeuInstruction::CsrAnd, VeuWriteType::Clear, true, true},
        {2, VeuInstruction::CsrWrite, VeuWriteType::Write, true, false},
        {3, VeuInstruction::CsrRead, VeuWriteType::Write, false, true},
    }};

    for (const auto &test : cases) {
        const auto decoded =
            decodeSpiritVeuInstruction(encodeCsr(0x105, 6, test.function3, 7));

        EXPECT_TRUE(decoded.valid);
        EXPECT_TRUE(decoded.csrInstruction);
        EXPECT_EQ(decoded.operation, test.operation);
        EXPECT_EQ(decoded.rd, 7);
        EXPECT_EQ(decoded.rs1, 6);
        EXPECT_TRUE(decoded.usesRs1);
        EXPECT_FALSE(decoded.usesRs2);
        EXPECT_FALSE(decoded.usesRs3);
        EXPECT_EQ(decoded.csrAddr, 0x105);
        EXPECT_TRUE(decoded.csrRead);
        EXPECT_EQ(decoded.csrWrite, test.csrWrite);
        EXPECT_EQ(decoded.writeType, test.writeType);
        EXPECT_EQ(decoded.writesRd, test.writesRd);
        EXPECT_EQ(decoded.veStart, 0);
    }
}

TEST(SpiritVeuDecodeTest, DecodesEveryTwoSourceVectorInstruction)
{
    struct Case
    {
        uint8_t function7;
        VeuInstruction operation;
        uint8_t startBit;
    };

    constexpr std::array<Case, 21> cases{{
        {0x00, VeuInstruction::Add, 0},
        {0x01, VeuInstruction::Sub, 1},
        {0x02, VeuInstruction::Min, 2},
        {0x03, VeuInstruction::Max, 3},
        {0x04, VeuInstruction::ReduceMin, 4},
        {0x05, VeuInstruction::ReduceMax, 5},
        {0x06, VeuInstruction::And, 6},
        {0x07, VeuInstruction::Or, 7},
        {0x08, VeuInstruction::Xor, 8},
        {0x09, VeuInstruction::SlideUp, 9},
        {0x0a, VeuInstruction::SlideDown, 10},
        {0x0b, VeuInstruction::Move, 11},
        {0x0c, VeuInstruction::ShiftRightLogical, 12},
        {0x0d, VeuInstruction::ShiftRightArithmetic, 13},
        {0x0e, VeuInstruction::NarrowClip, 14},
        {0x0f, VeuInstruction::WidenReduceSum, 15},
        {0x10, VeuInstruction::ReduceSum, 16},
        {0x11, VeuInstruction::Compress, 17},
        {0x14, VeuInstruction::Multiply, 20},
        {0x15, VeuInstruction::MultiplyHighSignedUnsigned, 21},
        {0x16, VeuInstruction::MultiplyHigh, 22},
    }};

    for (const auto &test : cases) {
        const auto decoded =
            decodeSpiritVeuInstruction(encodeVector(test.function7, 9, 8, 7));

        EXPECT_TRUE(decoded.valid);
        EXPECT_FALSE(decoded.csrInstruction);
        EXPECT_EQ(decoded.operation, test.operation);
        EXPECT_EQ(decoded.rd, 7);
        EXPECT_EQ(decoded.rs1, 8);
        EXPECT_EQ(decoded.rs2, 9);
        EXPECT_TRUE(decoded.usesRs1);
        EXPECT_TRUE(decoded.usesRs2);
        EXPECT_FALSE(decoded.usesRs3);
        EXPECT_TRUE(decoded.writesRd);
        EXPECT_EQ(
            decoded.csrAddr,
            static_cast<uint16_t>(VeuCsr::ReadAddress1));
        EXPECT_TRUE(decoded.csrRead);
        EXPECT_TRUE(decoded.csrWrite);
        EXPECT_EQ(decoded.writeType, VeuWriteType::VectorStart);
        EXPECT_EQ(decoded.veStart, uint32_t{1} << test.startBit);
    }
}

TEST(SpiritVeuDecodeTest, DecodesVmaddAndVmsubRs3)
{
    const auto vmsub =
        decodeSpiritVeuInstruction(encodeThreeSource(10, 0, 9, 8, 7));
    const auto vmadd =
        decodeSpiritVeuInstruction(encodeThreeSource(11, 1, 6, 5, 4));

    EXPECT_TRUE(vmsub.valid);
    EXPECT_EQ(vmsub.operation, VeuInstruction::MultiplySubtract);
    EXPECT_EQ(vmsub.rs3, 10);
    EXPECT_TRUE(vmsub.usesRs3);
    EXPECT_EQ(vmsub.veStart, uint32_t{1} << 18);

    EXPECT_TRUE(vmadd.valid);
    EXPECT_EQ(vmadd.operation, VeuInstruction::MultiplyAdd);
    EXPECT_EQ(vmadd.rd, 4);
    EXPECT_EQ(vmadd.rs1, 5);
    EXPECT_EQ(vmadd.rs2, 6);
    EXPECT_EQ(vmadd.rs3, 11);
    EXPECT_TRUE(vmadd.usesRs3);
    EXPECT_EQ(vmadd.veStart, uint32_t{1} << 19);
}

TEST(SpiritVeuDecodeTest, RejectsNormalAndUnsupportedInstructions)
{
    EXPECT_FALSE(decodeSpiritVeuInstruction(0x002081b3).valid);
    EXPECT_FALSE(decodeSpiritVeuInstruction(encodeVector(0x12, 2, 1, 3)).valid);
    EXPECT_FALSE(
        decodeSpiritVeuInstruction(encodeVector(0x00, 2, 1, 3) | (1u << 12))
            .valid);
}

} // namespace
} // namespace brs
} // namespace gem5
