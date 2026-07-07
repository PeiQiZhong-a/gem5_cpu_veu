#include "brs/veu/veu_cbu.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

VeuCbuIssue
makeIssue(VeuInstruction operation)
{
    VeuCbuIssue issue;
    issue.valid = true;
    issue.operation = operation;
    issue.csrAddr = static_cast<uint16_t>(VeuCsr::ReadAddress1);
    issue.csrRead = true;
    issue.csrWrite = true;
    issue.writeType = VeuWriteType::VectorStart;
    issue.veStart = veuStartMask(operation);
    issue.operand1 = 0x11112222;
    issue.operand2 = 0x33334444;
    issue.operand3 = 0x55556666;
    return issue;
}

TEST(VeuCbuTest, ResetIsIdleAndDrivesNoRequest)
{
    VeuCbu cbu;
    const auto output = cbu.evaluate({});

    EXPECT_EQ(cbu.state(), VeuCbu::State::Idle);
    EXPECT_TRUE(output.ready);
    EXPECT_FALSE(output.busy);
    EXPECT_FALSE(output.complete);
    EXPECT_FALSE(output.request.hasTransaction());
}

TEST(VeuCbuTest, SingleShotRequestIsHeldUntilResponse)
{
    VeuCbu cbu;
    const auto issue = makeIssue(VeuInstruction::Add);

    cbu.clock(issue, {});

    auto output = cbu.evaluate({});
    ASSERT_EQ(cbu.state(), VeuCbu::State::SendFirst);
    EXPECT_FALSE(output.ready);
    EXPECT_TRUE(output.busy);
    EXPECT_FALSE(output.complete);
    EXPECT_EQ(output.request.csrAddr, issue.csrAddr);
    EXPECT_TRUE(output.request.csrRead);
    EXPECT_TRUE(output.request.csrWrite);
    EXPECT_EQ(
        output.request.writeType,
        static_cast<uint8_t>(VeuWriteType::VectorStart));
    EXPECT_EQ(
        output.request.writeData,
        packVeuOperands(issue.operand1, issue.operand2));
    EXPECT_EQ(output.request.veStart, uint32_t{1} << 0);

    cbu.clock({}, {});
    output = cbu.evaluate({});
    EXPECT_EQ(cbu.state(), VeuCbu::State::SendFirst);
    EXPECT_EQ(
        output.request.writeData,
        packVeuOperands(issue.operand1, issue.operand2));

    const VeuResponse response{true, 0xa5a55a5a};
    output = cbu.evaluate(response);
    EXPECT_TRUE(output.busy);
    EXPECT_TRUE(output.complete);
    EXPECT_EQ(output.result, response.readData);
    EXPECT_TRUE(output.request.hasTransaction());

    cbu.clock({}, response);
    output = cbu.evaluate({});
    EXPECT_EQ(cbu.state(), VeuCbu::State::Idle);
    EXPECT_TRUE(output.ready);
    EXPECT_FALSE(output.busy);
    EXPECT_FALSE(output.complete);
    EXPECT_FALSE(output.request.hasTransaction());
}

TEST(VeuCbuTest, VmaddUsesTwoResponseHandshakes)
{
    VeuCbu cbu;
    const auto issue = makeIssue(VeuInstruction::MultiplyAdd);
    cbu.clock(issue, {});

    auto output = cbu.evaluate({});
    ASSERT_EQ(cbu.state(), VeuCbu::State::SendFirst);
    EXPECT_EQ(
        output.request.writeData,
        packVeuOperands(issue.operand1, issue.operand2));
    EXPECT_EQ(output.request.veStart, uint32_t{1} << 19);

    const VeuResponse firstResponse{true, 0x11111111};
    output = cbu.evaluate(firstResponse);
    EXPECT_TRUE(output.busy);
    EXPECT_FALSE(output.complete);

    cbu.clock({}, firstResponse);
    output = cbu.evaluate({});
    ASSERT_EQ(cbu.state(), VeuCbu::State::WaitSecond);
    EXPECT_TRUE(output.busy);
    EXPECT_EQ(
        output.request.writeData,
        packVeuOperands(issue.operand3, issue.operand3));

    const VeuResponse secondResponse{true, 0x22222222};
    output = cbu.evaluate(secondResponse);
    EXPECT_TRUE(output.complete);
    EXPECT_EQ(output.result, secondResponse.readData);

    cbu.clock({}, secondResponse);
    EXPECT_EQ(cbu.state(), VeuCbu::State::Idle);
}

TEST(VeuCbuTest, VmsubAlsoUsesTwoResponseHandshakes)
{
    VeuCbu cbu;
    const auto issue = makeIssue(VeuInstruction::MultiplySubtract);
    cbu.clock(issue, {});

    EXPECT_EQ(cbu.state(), VeuCbu::State::SendFirst);
    EXPECT_EQ(cbu.evaluate({}).request.veStart, uint32_t{1} << 18);

    const VeuResponse response{true, 0};
    EXPECT_FALSE(cbu.evaluate(response).complete);
    cbu.clock({}, response);
    EXPECT_EQ(cbu.state(), VeuCbu::State::WaitSecond);
    EXPECT_TRUE(cbu.evaluate(response).complete);
}

TEST(VeuCbuTest, NewIssueIsIgnoredWhileBusy)
{
    VeuCbu cbu;
    const auto first = makeIssue(VeuInstruction::Add);
    auto second = makeIssue(VeuInstruction::Sub);
    second.operand1 = 0xaaaaaaaa;
    second.operand2 = 0xbbbbbbbb;

    cbu.clock(first, {});
    cbu.clock(second, {});

    const auto output = cbu.evaluate({});
    EXPECT_EQ(cbu.state(), VeuCbu::State::SendFirst);
    EXPECT_EQ(output.request.veStart, uint32_t{1} << 0);
    EXPECT_EQ(
        output.request.writeData,
        packVeuOperands(first.operand1, first.operand2));
}

} // namespace
} // namespace brs
} // namespace gem5
