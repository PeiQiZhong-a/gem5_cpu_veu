#include "brs/veu/fake_veu.hh"
#include "brs/veu/veu_cbu.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

VeuRequest
makeRequest(uint64_t data = 0x2222222211111111ULL)
{
    VeuRequest request;
    request.csrAddr = static_cast<uint16_t>(VeuCsr::ReadAddress1);
    request.csrRead = true;
    request.csrWrite = true;
    request.writeType = static_cast<uint8_t>(VeuWriteType::VectorStart);
    request.writeData = data;
    request.veStart = 1;
    return request;
}

VeuCbuIssue
makeVmaddIssue()
{
    VeuCbuIssue issue;
    issue.valid = true;
    issue.operation = VeuInstruction::MultiplyAdd;
    issue.csrAddr = static_cast<uint16_t>(VeuCsr::ReadAddress1);
    issue.csrRead = true;
    issue.csrWrite = true;
    issue.writeType = VeuWriteType::VectorStart;
    issue.veStart = veuStartMask(issue.operation);
    issue.operand1 = 0x11111111;
    issue.operand2 = 0x22222222;
    issue.operand3 = 0x33333333;
    return issue;
}

TEST(FakeVeuTest, MinimumLatencyProducesOneCycleResponsePulse)
{
    FakeVeu fake(1, 0x12345678);
    const auto request = makeRequest();

    EXPECT_FALSE(fake.evaluate().valid);
    fake.clock(request);

    EXPECT_EQ(fake.state(), FakeVeu::State::Responding);
    EXPECT_TRUE(fake.evaluate().valid);
    EXPECT_EQ(fake.evaluate().readData, 0x12345678);
    EXPECT_EQ(fake.acceptedRequestCount(), 1);

    fake.clock(request);
    EXPECT_EQ(fake.state(), FakeVeu::State::Recovery);
    EXPECT_FALSE(fake.evaluate().valid);
    EXPECT_EQ(fake.responseCount(), 1);

    fake.clock({});
    EXPECT_EQ(fake.state(), FakeVeu::State::Idle);
    EXPECT_FALSE(fake.evaluate().valid);
    EXPECT_EQ(fake.acceptedRequestCount(), 1);
}

TEST(FakeVeuTest, ConfiguredLatencyCountsFromRequestSamplingEdge)
{
    FakeVeu fake(3, 0xa5a55a5a);
    const auto request = makeRequest();

    fake.clock(request);
    EXPECT_FALSE(fake.evaluate().valid);

    fake.clock(request);
    EXPECT_FALSE(fake.evaluate().valid);
    EXPECT_EQ(fake.acceptedRequestCount(), 1);

    fake.clock(request);
    EXPECT_TRUE(fake.evaluate().valid);
    EXPECT_EQ(fake.evaluate().readData, 0xa5a55a5a);
    EXPECT_EQ(fake.acceptedRequestCount(), 1);
}

TEST(FakeVeuTest, HeldRequestIsNotReacceptedBeforeItsResponse)
{
    FakeVeu fake(4);
    const auto request = makeRequest();

    fake.clock(request);
    for (int cycle = 0; cycle < 3; ++cycle) {
        EXPECT_EQ(fake.acceptedRequestCount(), 1);
        fake.clock(request);
    }

    EXPECT_TRUE(fake.evaluate().valid);
    EXPECT_EQ(fake.acceptedRequestCount(), 1);
}

TEST(FakeVeuTest, RecoveryAcceptsVmaddSecondPhase)
{
    FakeVeu fake(1);
    const auto first = makeRequest(0x2222222211111111ULL);
    const auto second = makeRequest(0x3333333333333333ULL);

    fake.clock(first);
    ASSERT_TRUE(fake.evaluate().valid);
    fake.clock(first);
    ASSERT_EQ(fake.state(), FakeVeu::State::Recovery);

    fake.clock(second);
    EXPECT_EQ(fake.acceptedRequestCount(), 2);
    EXPECT_EQ(fake.lastAcceptedRequest().writeData, second.writeData);
    EXPECT_TRUE(fake.evaluate().valid);
}

TEST(FakeVeuTest, RunsTwoShotCbuHandshakeEndToEnd)
{
    VeuCbu cbu;
    FakeVeu fake(1, 0xc001cafe);
    const auto issue = makeVmaddIssue();
    int completeCount = 0;
    uint32_t completionResult = 0;

    for (int cycle = 0; cycle < 8; ++cycle) {
        const VeuResponse response = fake.evaluate();
        const VeuCbuOutput cbuOutput = cbu.evaluate(response);

        if (cbuOutput.complete) {
            ++completeCount;
            completionResult = cbuOutput.result;
        }

        cbu.clock(cycle == 0 ? issue : VeuCbuIssue{}, response);
        fake.clock(cbuOutput.request);
    }

    EXPECT_EQ(fake.acceptedRequestCount(), 2);
    EXPECT_EQ(fake.responseCount(), 2);
    EXPECT_EQ(completeCount, 1);
    EXPECT_EQ(completionResult, 0xc001cafe);
    EXPECT_FALSE(cbu.busy());
}

TEST(FakeVeuTest, ZeroLatencyConfigurationIsClampedToOneCycle)
{
    FakeVeu fake(0);
    EXPECT_EQ(fake.responseLatencyCycles(), 1);
}

} // namespace
} // namespace brs
} // namespace gem5
