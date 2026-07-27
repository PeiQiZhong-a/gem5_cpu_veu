#include "brs/memory/sram_converter_32to256.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

TEST(SramConverter32To256Test, PlacesWordAndStrobesInAddressSelectedLane)
{
    SramConverter32To256 converter;
    converter.reset();
    Sram32Request request;
    request.valid = true;
    request.address = 0x29120014;
    request.writeStrobe = 0x5;
    request.writeData = 0xaabbccdd;

    converter.clock(request, {});
    const auto output = converter.evaluate();
    ASSERT_EQ(converter.state(), SramConverter32To256::State::WaitAck);
    ASSERT_TRUE(output.sram.valid);
    EXPECT_EQ(output.sram.address, 0x29120000);
    EXPECT_EQ(output.sram.writeStrobe, uint32_t{0x5} << 20);
    EXPECT_EQ(output.sram.writeData[20], 0xdd);
    EXPECT_EQ(output.sram.writeData[21], 0xcc);
    EXPECT_EQ(output.sram.writeData[22], 0xbb);
    EXPECT_EQ(output.sram.writeData[23], 0xaa);
}

TEST(SramConverter32To256Test, HoldsRequestUntilAckThenPulsesReady)
{
    SramConverter32To256 converter;
    converter.reset();
    Sram32Request request{true, 0x2912001c, 0, 0};
    converter.clock(request, {});
    const auto expected = converter.evaluate().sram;

    for (int cycle = 0; cycle < 3; ++cycle) {
        converter.clock({}, {});
        const auto held = converter.evaluate();
        EXPECT_TRUE(held.sram.valid);
        EXPECT_EQ(held.sram.address, expected.address);
        EXPECT_EQ(held.sram.writeStrobe, expected.writeStrobe);
        EXPECT_FALSE(held.master.valid);
    }

    Sram256Response response;
    response.valid = true;
    response.readData[28] = 0x78;
    response.readData[29] = 0x56;
    response.readData[30] = 0x34;
    response.readData[31] = 0x12;
    converter.clock({}, response);
    auto output = converter.evaluate();
    EXPECT_EQ(converter.state(), SramConverter32To256::State::Idle);
    EXPECT_FALSE(output.sram.valid);
    EXPECT_TRUE(output.master.valid);
    EXPECT_FALSE(output.master.isWrite);
    EXPECT_EQ(output.master.readData, 0x12345678);

    converter.clock({}, {});
    EXPECT_FALSE(converter.evaluate().master.valid);
}

TEST(SramConverter32To256Test, WriteCompletionReturnsZero)
{
    SramConverter32To256 converter;
    converter.reset();
    converter.clock({true, 0x29120000, 0xf, 0xdeadbeef}, {});
    Sram256Response response;
    response.valid = true;
    response.readData.fill(0xff);
    converter.clock({}, response);
    const auto output = converter.evaluate();
    EXPECT_TRUE(output.master.valid);
    EXPECT_TRUE(output.master.isWrite);
    EXPECT_EQ(output.master.readData, 0);
}

} // namespace
} // namespace brs
} // namespace gem5
