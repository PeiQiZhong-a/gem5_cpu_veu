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
    decoded.sau_operation = brs::SauInstruction::Set7;
    decoded.sau_csr_addr = 0x20c;
    decoded.sau_csr_write = true;
    decoded.sau_write_type = brs::SauWriteType::Set;

    const auto issue =
        makeSauCbuIssue(decoded, 0x11223344, 0x55667788);

    EXPECT_TRUE(issue.valid);
    EXPECT_EQ(issue.operation, brs::SauInstruction::Set7);
    EXPECT_EQ(issue.csrAddr, 0x20c);
    EXPECT_FALSE(issue.csrRead);
    EXPECT_TRUE(issue.csrWrite);
    EXPECT_EQ(issue.writeType, brs::SauWriteType::Set);
    EXPECT_EQ(issue.veStart, 0);
    EXPECT_EQ(issue.operand1, 0x11223344);
    EXPECT_EQ(issue.operand2, 0x55667788);
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
