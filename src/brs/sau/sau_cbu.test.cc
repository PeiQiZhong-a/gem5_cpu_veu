#include "brs/sau/sau_cbu.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

SauCbuIssue
makeIssue()
{
    SauCbuIssue issue;
    issue.valid = true;
    issue.operation = SauInstruction::Set3;
    issue.csrAddr = sauCsrAddress(issue.operation);
    issue.csrWrite = true;
    issue.writeType = SauWriteType::Set;
    issue.operand1 = 0x11223344;
    issue.operand2 = 0x55667788;
    return issue;
}

TEST(SauCbuTest, ResetIsIdle)
{
    SauCbu cbu;
    const SauCbuOutput output = cbu.evaluate({});

    EXPECT_TRUE(output.ready);
    EXPECT_FALSE(output.busy);
    EXPECT_FALSE(output.complete);
    EXPECT_FALSE(output.request.hasTransaction());
}

TEST(SauCbuTest, HoldsRequestUntilValidResponse)
{
    SauCbu cbu;
    const SauCbuIssue issue = makeIssue();

    cbu.clock(issue, {});
    const SauCbuOutput first = cbu.evaluate({});
    ASSERT_TRUE(first.busy);
    EXPECT_FALSE(first.complete);
    EXPECT_EQ(first.request.csrAddr, 0x204);
    EXPECT_TRUE(first.request.csrWrite);
    EXPECT_EQ(first.request.writeType,
              static_cast<uint8_t>(SauWriteType::Set));
    EXPECT_EQ(first.request.writeData, 0x5566778811223344ULL);

    cbu.clock({}, {});
    const SauCbuOutput held = cbu.evaluate({});
    EXPECT_EQ(held.request.csrAddr, first.request.csrAddr);
    EXPECT_EQ(held.request.writeData, first.request.writeData);

    SauResponse response;
    response.valid = true;
    response.readData = 0xc001cafe;
    const SauCbuOutput completed = cbu.evaluate(response);
    EXPECT_TRUE(completed.complete);
    EXPECT_EQ(completed.result, 0xc001cafe);

    cbu.clock({}, response);
    EXPECT_EQ(cbu.state(), SauCbu::State::Idle);
}

TEST(SauCbuTest, IgnoresSecondIssueWhileBusy)
{
    SauCbu cbu;
    SauCbuIssue first = makeIssue();
    SauCbuIssue second = first;
    second.csrAddr = sauCsrAddress(SauInstruction::Set4);

    cbu.clock(first, {});
    cbu.clock(second, {});

    EXPECT_EQ(cbu.evaluate({}).request.csrAddr, first.csrAddr);
}

} // namespace
} // namespace brs
} // namespace gem5
