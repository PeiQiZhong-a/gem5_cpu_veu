#include "brs/memory/sram_converter_32to128.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

TEST(SramConverter32To128Test, FollowsIdleConvertWaitAckRegisters)
{
    SramConverter32To128 converter;
    converter.reset();

    Sram32Request request;
    request.valid = true;
    request.address = 0x2001000c;
    request.writeStrobe = 0xf;
    request.writeData = 0x44332211;

    converter.clock(request, {});
    EXPECT_EQ(converter.state(), SramConverter32To128::State::Convert);
    EXPECT_FALSE(converter.evaluate().sram.valid);

    converter.clock({}, {});
    EXPECT_EQ(converter.state(), SramConverter32To128::State::WaitAck);
    const auto converted = converter.evaluate();
    EXPECT_TRUE(converted.sram.valid);
    EXPECT_EQ(converted.sram.address, 0x20010000u);
    EXPECT_EQ(converted.sram.writeStrobe, 0xf000u);
    EXPECT_EQ(converted.sram.writeData[12], 0x11);
    EXPECT_EQ(converted.sram.writeData[15], 0x44);

    converter.clock({}, {});
    EXPECT_TRUE(converter.evaluate().sram.valid);
    EXPECT_FALSE(converter.evaluate().master.valid);

    Sram128Response response;
    response.valid = true;
    converter.clock({}, response);
    EXPECT_EQ(converter.state(), SramConverter32To128::State::Idle);
    EXPECT_FALSE(converter.evaluate().sram.valid);
    EXPECT_TRUE(converter.evaluate().master.valid);
    EXPECT_TRUE(converter.evaluate().master.isWrite);

    converter.clock({}, {});
    EXPECT_FALSE(converter.evaluate().master.valid);
}

TEST(SramConverter32To128Test, SelectsOriginalReadWord)
{
    SramConverter32To128 converter;
    converter.reset();
    converter.clock({true, 0x20010008, 0, 0}, {});
    converter.clock({}, {});

    Sram128Response response;
    response.valid = true;
    response.readData[8] = 0x78;
    response.readData[9] = 0x56;
    response.readData[10] = 0x34;
    response.readData[11] = 0x12;
    converter.clock({}, response);

    ASSERT_TRUE(converter.evaluate().master.valid);
    EXPECT_FALSE(converter.evaluate().master.isWrite);
    EXPECT_EQ(converter.evaluate().master.readData, 0x12345678u);
}

} // namespace
} // namespace brs
} // namespace gem5
