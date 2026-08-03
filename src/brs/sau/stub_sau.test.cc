#include "brs/sau/stub_sau.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

SauResponse
runTransaction(
    StubSau &sau,
    const SauRequest &request,
    unsigned maxCycles = 16)
{
    for (unsigned cycle = 0; cycle < maxCycles; ++cycle) {
        const SauResponse response = sau.evaluate();
        if (response.valid) {
            sau.clock(request);
            return response;
        }
        sau.clock(request);
    }
    return {};
}

TEST(StubSauTest, WritesAndReadsAllFourRegisterPairs)
{
    StubSau sau(1);

    for (uint8_t slot = 1; slot <= SauSlotCount; ++slot) {
        SauRequest write;
        write.csrAddr = SauCsrBase + (slot - 1) * 2;
        write.csrWrite = true;
        write.writeType = static_cast<uint8_t>(SauWriteType::Set);
        write.writeData =
            (static_cast<uint64_t>(0x20000000u + slot) << 32) |
            (0x10000000u + slot);
        ASSERT_TRUE(runTransaction(sau, write).valid);
        sau.clock({});

        SauRequest readLow;
        readLow.csrAddr = write.csrAddr;
        readLow.csrRead = true;
        ASSERT_EQ(runTransaction(sau, readLow).readData,
                  0x10000000u + slot);
        sau.clock({});

        SauRequest readHigh = readLow;
        ++readHigh.csrAddr;
        ASSERT_EQ(runTransaction(sau, readHigh).readData,
                  0x20000000u + slot);
        sau.clock({});
    }

    EXPECT_EQ(sau.acceptedRequestCount(), SauSlotCount * 3);
    EXPECT_EQ(sau.responseCount(), SauSlotCount * 3);
}

TEST(StubSauTest, EmitsOneValidPulseAndDoesNotDuplicateHeldRequest)
{
    StubSau sau(3);
    SauRequest request;
    request.csrAddr = 0x200;
    request.csrWrite = true;
    request.writeData = 0x0123456789abcdefULL;

    unsigned validCycles = 0;
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        if (sau.evaluate().valid) {
            ++validCycles;
        }
        sau.clock(cycle < 6 ? request : SauRequest{});
    }

    EXPECT_EQ(validCycles, 1);
    EXPECT_EQ(sau.acceptedRequestCount(), 1);
    EXPECT_EQ(sau.responseCount(), 1);
    EXPECT_EQ(sau.slotValue(1), request.writeData);
}

TEST(StubSauTest, ResetClearsProtocolAndRegisterState)
{
    StubSau sau;
    SauRequest request;
    request.csrAddr = sauCsrAddress(SauInstruction::Set4);
    request.csrWrite = true;
    request.writeData = 0xffffffffffffffffULL;
    sau.clock(request);

    sau.reset();

    EXPECT_EQ(sau.state(), StubSau::State::Idle);
    EXPECT_EQ(sau.acceptedRequestCount(), 0);
    EXPECT_EQ(sau.slotValue(4), 0);
    EXPECT_FALSE(sau.evaluate().valid);
}

} // namespace
} // namespace brs
} // namespace gem5
