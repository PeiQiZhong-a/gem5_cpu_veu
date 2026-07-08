#include "brs/pipeline/veu_issue.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace
{

TEST(VeuIssueTest, CopiesDecodedControlAndForwardedOperands)
{
    IDEX decoded;
    decoded.valid = true;
    decoded.kind = InstrKind::VEU;
    decoded.veu_operation = brs::VeuInstruction::MultiplyAdd;
    decoded.veu_csr_addr =
        static_cast<uint16_t>(brs::VeuCsr::ReadAddress1);
    decoded.veu_csr_read = true;
    decoded.veu_csr_write = true;
    decoded.veu_write_type = brs::VeuWriteType::VectorStart;
    decoded.veu_start =
        brs::veuStartMask(brs::VeuInstruction::MultiplyAdd);

    const auto issue = makeVeuCbuIssue(
        decoded, 0x11111111, 0x22222222, 0x33333333);

    EXPECT_TRUE(issue.valid);
    EXPECT_EQ(issue.operation, brs::VeuInstruction::MultiplyAdd);
    EXPECT_EQ(
        issue.csrAddr,
        static_cast<uint16_t>(brs::VeuCsr::ReadAddress1));
    EXPECT_TRUE(issue.csrRead);
    EXPECT_TRUE(issue.csrWrite);
    EXPECT_EQ(issue.writeType, brs::VeuWriteType::VectorStart);
    EXPECT_EQ(issue.veStart, uint32_t{1} << 19);
    EXPECT_EQ(issue.operand1, 0x11111111);
    EXPECT_EQ(issue.operand2, 0x22222222);
    EXPECT_EQ(issue.operand3, 0x33333333);
}

TEST(VeuIssueTest, RejectsNonVeuPipelineEntry)
{
    IDEX decoded;
    decoded.valid = true;
    decoded.kind = InstrKind::ADD;

    EXPECT_FALSE(makeVeuCbuIssue(decoded, 1, 2, 3).valid);
}

} // namespace
} // namespace gem5
