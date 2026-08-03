#include "brs/pipeline/sau_issue.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace
{

TEST(SauIssueTest, CopiesDecodedControlAndForwardedOperands)
{
    IDEX decoded;
    decoded.valid = true;
    decoded.kind = InstrKind::SAU;
    decoded.sau_operation = brs::SauInstruction::Set4;
    decoded.sau_csr_addr = 0x206;
    decoded.sau_csr_write = true;
    decoded.sau_write_type = brs::SauWriteType::Set;

    const auto issue =
        makeSauCbuIssue(decoded, 0x11223344, 0x55667788);

    EXPECT_TRUE(issue.valid);
    EXPECT_EQ(issue.operation, brs::SauInstruction::Set4);
    EXPECT_EQ(issue.csrAddr, 0x206);
    EXPECT_FALSE(issue.csrRead);
    EXPECT_TRUE(issue.csrWrite);
    EXPECT_EQ(issue.writeType, brs::SauWriteType::Set);
    EXPECT_EQ(issue.veStart, 0);
    EXPECT_EQ(issue.operand1, 0x11223344);
    EXPECT_EQ(issue.operand2, 0x55667788);
}

TEST(SauIssueTest, PacksArchiveSetOperandsAsRs2ThenRs1)
{
    const brs::SauDecodeInfo archiveSet =
        brs::decodeSpiritSauInstruction(0x00ab906b);
    IDEX decoded;
    decoded.valid = archiveSet.valid;
    decoded.kind = InstrKind::SAU;
    decoded.sau_operation = archiveSet.operation;
    decoded.sau_csr_addr = archiveSet.csrAddr;
    decoded.sau_csr_read = archiveSet.csrRead;
    decoded.sau_csr_write = archiveSet.csrWrite;
    decoded.sau_write_type = archiveSet.writeType;

    const auto issue =
        makeSauHcCbuIssue(decoded, 0x11223344, 0x55667788);

    ASSERT_TRUE(issue.valid);
    EXPECT_EQ(issue.firstRequest.csrAddr, brs::SauCsrBase);
    EXPECT_TRUE(issue.firstRequest.csrWrite);
    EXPECT_FALSE(issue.firstRequest.csrRead);
    EXPECT_EQ(issue.firstRequest.writeData, 0x5566778811223344ULL);
}

TEST(SauIssueTest, RejectsNonSauPipelineEntry)
{
    IDEX decoded;
    decoded.valid = true;
    decoded.kind = InstrKind::VEU;

    EXPECT_FALSE(makeSauCbuIssue(decoded, 1, 2).valid);
}

} // namespace
} // namespace gem5
