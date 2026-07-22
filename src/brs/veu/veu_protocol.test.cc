#include "brs/veu/cycle_trace.hh"
#include "brs/veu/veu_protocol.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

TEST(VeuProtocolTest, MatchesAerithDataPathGeometry)
{
    EXPECT_EQ(VeuVectorBits, 256);
    EXPECT_EQ(VeuVectorBytes, 32);
    EXPECT_EQ(VeuLaneBits, 32);
    EXPECT_EQ(VeuLaneCount, 8);
    EXPECT_EQ(VeuComputeDelayCycles, 3);
    EXPECT_EQ(VeuTcmDelayCycles, 3);
    EXPECT_EQ(VeuLoadReturnLatencyCycles, 4);
}

TEST(VeuProtocolTest, RecognizesAerithCsrWindow)
{
    EXPECT_TRUE(isVeuCsr(0x100));
    EXPECT_TRUE(isVeuCsr(0x107));
    EXPECT_FALSE(isVeuCsr(0x0ff));
    EXPECT_FALSE(isVeuCsr(0x108));
}

TEST(VeuProtocolTest, PacksOperandsLikeSpiritCbu)
{
    constexpr uint32_t op1 = 0x11223344;
    constexpr uint32_t op2 = 0xaabbccdd;
    constexpr uint64_t packed = packVeuOperands(op1, op2);

    EXPECT_EQ(packed, 0xaabbccdd11223344ULL);
    EXPECT_EQ(unpackVeuOperand1(packed), op1);
    EXPECT_EQ(unpackVeuOperand2(packed), op2);
}

TEST(VeuProtocolTest, AlignsVectorLengthLikeAerithVcu)
{
    EXPECT_EQ(alignVeuLengthBits(0), 0);
    EXPECT_EQ(alignVeuLengthBits(1), 256);
    EXPECT_EQ(alignVeuLengthBits(256), 256);
    EXPECT_EQ(alignVeuLengthBits(257), 512);
    EXPECT_EQ(effectiveVeuLengthAtStart(0), 0);
}

TEST(VeuProtocolTest, DecodesSpiritCsrInstructions)
{
    EXPECT_EQ(decodeVeuInstruction(0x0000000b), VeuInstruction::CsrOr);
    EXPECT_EQ(decodeVeuInstruction(0x0000100b), VeuInstruction::CsrAnd);
    EXPECT_EQ(decodeVeuInstruction(0x0000200b), VeuInstruction::CsrWrite);
    EXPECT_EQ(decodeVeuInstruction(0x0000300b), VeuInstruction::CsrRead);
}

TEST(VeuProtocolTest, DecodesSpiritVectorInstructions)
{
    EXPECT_EQ(decodeVeuInstruction(0x0000006b), VeuInstruction::Add);
    EXPECT_EQ(decodeVeuInstruction(0x0200006b), VeuInstruction::Sub);
    EXPECT_EQ(decodeVeuInstruction(0x1c00006b), VeuInstruction::NarrowClip);
    EXPECT_EQ(decodeVeuInstruction(0x2800006b), VeuInstruction::Multiply);
    EXPECT_EQ(decodeVeuInstruction(0x2c00006b), VeuInstruction::MultiplyHigh);
}

TEST(VeuProtocolTest, DecodesSpiritThreeSourceInstructions)
{
    EXPECT_EQ(
        decodeVeuInstruction(0x0200002b),
        VeuInstruction::MultiplySubtract);
    EXPECT_EQ(
        decodeVeuInstruction(0x0200102b),
        VeuInstruction::MultiplyAdd);
    EXPECT_TRUE(isTwoShotVeuInstruction(VeuInstruction::MultiplySubtract));
    EXPECT_TRUE(isTwoShotVeuInstruction(VeuInstruction::MultiplyAdd));
}

TEST(VeuProtocolTest, MapsInstructionsToSpiritVeStartBits)
{
    EXPECT_EQ(veuStartMask(VeuInstruction::Add), 1u << 0);
    EXPECT_EQ(veuStartMask(VeuInstruction::NarrowClip), 1u << 14);
    EXPECT_EQ(veuStartMask(VeuInstruction::MultiplySubtract), 1u << 18);
    EXPECT_EQ(veuStartMask(VeuInstruction::MultiplyAdd), 1u << 19);
    EXPECT_EQ(veuStartMask(VeuInstruction::Multiply), 1u << 20);
    EXPECT_EQ(veuStartMask(VeuInstruction::MultiplyHigh), 1u << 22);
    EXPECT_EQ(veuStartMask(VeuInstruction::Unknown), 0u);
}

TEST(CycleTraceTest, UsesSharedEventNames)
{
    EXPECT_STREQ(cycleTraceEventName(CycleTraceEvent::OperationStart),
                 "operation_start");
    EXPECT_STREQ(cycleTraceEventName(CycleTraceEvent::ReadRequest),
                 "read_request");
    EXPECT_STREQ(cycleTraceEventName(CycleTraceEvent::OperationFinish),
                 "operation_finish");
}

} // anonymous namespace
} // namespace brs
} // namespace gem5
