#include "brs/hc/hc_cbu.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

HcCbuIssue
makeIssue(bool twoShot = false)
{
    HcCbuIssue issue;
    issue.valid = true;
    issue.firstRequest.csrAddr = 0x200;
    issue.firstRequest.csrWrite = true;
    issue.firstRequest.writeType = static_cast<uint8_t>(HcWriteType::Set);
    issue.firstRequest.writeData = 0x2222222211111111ULL;
    issue.twoShot = twoShot;
    issue.secondWriteData = 0x3333333333333333ULL;
    return issue;
}

TEST(HcCbuTest, HoldsEveryRequestFieldUntilSelectedEndpointResponds)
{
    for (uint32_t latency = 1; latency <= 5; ++latency) {
        HcCbu cbu;
        const auto issue = makeIssue();
        cbu.clock(issue, {});
        const auto expected = cbu.evaluate({}).request;

        for (uint32_t cycle = 0; cycle < latency; ++cycle) {
            const auto held = cbu.evaluate({});
            EXPECT_TRUE(held.busy);
            EXPECT_FALSE(held.complete);
            EXPECT_EQ(held.request.csrAddr, expected.csrAddr);
            EXPECT_EQ(held.request.csrRead, expected.csrRead);
            EXPECT_EQ(held.request.csrWrite, expected.csrWrite);
            EXPECT_EQ(held.request.writeType, expected.writeType);
            EXPECT_EQ(held.request.writeData, expected.writeData);
            EXPECT_EQ(held.request.veStart, expected.veStart);
            cbu.clock({}, {});
        }

        const HcResponse response{true, 0x12340000u + latency};
        const auto completed = cbu.evaluate(response);
        EXPECT_TRUE(completed.complete);
        EXPECT_EQ(completed.result, response.readData);
        cbu.clock({}, response);
        EXPECT_TRUE(cbu.evaluate({}).ready);
    }
}

TEST(HcCbuTest, TwoShotCompletesOnlyOnSecondResponse)
{
    HcCbu cbu;
    cbu.clock(makeIssue(true), {});

    const HcResponse first{true, 0x11111111};
    EXPECT_FALSE(cbu.evaluate(first).complete);
    cbu.clock({}, first);
    EXPECT_EQ(cbu.state(), HcCbu::State::WaitSecond);
    EXPECT_EQ(cbu.evaluate({}).request.writeData, 0x3333333333333333ULL);

    const HcResponse second{true, 0x22222222};
    EXPECT_TRUE(cbu.evaluate(second).complete);
    EXPECT_EQ(cbu.evaluate(second).result, second.readData);
    cbu.clock({}, second);
    EXPECT_EQ(cbu.state(), HcCbu::State::Idle);
}

TEST(HcCbuTest, RejectsIssueWithoutReadOrWrite)
{
    HcCbu cbu;
    auto issue = makeIssue();
    issue.firstRequest.csrWrite = false;
    cbu.clock(issue, {});
    EXPECT_TRUE(cbu.evaluate({}).ready);
}

} // namespace
} // namespace brs
} // namespace gem5
