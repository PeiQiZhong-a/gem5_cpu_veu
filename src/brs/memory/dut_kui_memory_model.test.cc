#include "brs/memory/dut_kui_memory_model.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

TEST(DutKuiMemoryModelTest, KeepsIbusOnIndependent128BitPath)
{
    DutKuiMemoryModel model;
    for (uint32_t word = 0; word < 4; ++word) {
        model.writeWord(word * 4, 0x100u + word);
    }
    ASSERT_TRUE(model.acceptIbus({0xcu}));
    EXPECT_FALSE(model.acceptIbus({0x10u}));

    DutKuiMemoryOutputs outputs;
    // The first edge captures/issues the request; latency is counted after it.
    for (int cycle = 0; cycle < 4; ++cycle) {
        outputs = model.clock(false);
    }
    ASSERT_TRUE(outputs.ibus.valid);
    EXPECT_EQ(outputs.ibus.readData[0], 0x100u);
    EXPECT_EQ(outputs.ibus.readData[3], 0x103u);
}

TEST(DutKuiMemoryModelTest, ConvertsDbusWordUsingAddressBitsFourToTwo)
{
    DutKuiMemoryModel model;
    constexpr uint32_t address = 0x29120014;
    DutKuiDbusRequest write{address, 0xf, 0xaabbccdd};
    ASSERT_TRUE(model.acceptDbus(write, false));
    model.clock(false);
    EXPECT_EQ(model.readWord(address), 0xaabbccddu);
    EXPECT_TRUE(model.dbusHasOutstanding());
    EXPECT_FALSE(model.acceptDbus({0x29120000, 0, 0}, false));

    DutKuiMemoryOutputs outputs;
    for (int cycle = 0; cycle < 4; ++cycle) {
        outputs = model.clock(false);
    }
    ASSERT_TRUE(outputs.dbus.valid);
    ASSERT_TRUE(model.acceptDbus({address, 0, 0}, false));
    for (int cycle = 0; cycle < 5; ++cycle) {
        outputs = model.clock(false);
    }
    ASSERT_TRUE(outputs.dbus.valid);
    EXPECT_EQ(outputs.dbus.readData, 0xaabbccddu);
}

TEST(DutKuiMemoryModelTest, Honors256BitVeuWriteStrobe)
{
    DutKuiMemoryModel model;
    DutKuiVeuRequest request;
    request.transactionId = 7;
    request.address = 0x29120020;
    request.isWrite = true;
    request.writeStrobe = (uint32_t{1} << 0) | (uint32_t{1} << 31);
    request.data[0] = 0x12;
    request.data[31] = 0xfe;
    ASSERT_TRUE(model.acceptVeu(request));
    const auto outputs = model.clock(true);
    EXPECT_EQ(model.readByte(0x29120020), 0x12);
    EXPECT_EQ(model.readByte(0x2912003f), 0xfe);
    EXPECT_EQ(model.readByte(0x29120021), 0x00);

    ASSERT_TRUE(outputs.veuWrite.valid);
    EXPECT_TRUE(outputs.veuWrite.isWrite);
    EXPECT_EQ(outputs.veuWrite.transactionId, 7u);
}

TEST(DutKuiMemoryModelTest, ReturnsVeuReadsAfterFourCyclesInOrder)
{
    DutKuiMemoryModel model;
    model.writeByte(0x29120020, 0x11);
    model.writeByte(0x29120040, 0x22);

    ASSERT_TRUE(model.acceptVeu({1, 0x29120020, false, 0, {}}));
    EXPECT_FALSE(model.acceptVeu({2, 0x29120040, false, 0, {}}));
    EXPECT_FALSE(model.clock(true).veuRead.valid);
    ASSERT_TRUE(model.acceptVeu({2, 0x29120040, false, 0, {}}));
    EXPECT_FALSE(model.clock(true).veuRead.valid);
    EXPECT_FALSE(model.clock(true).veuRead.valid);
    auto outputs = model.clock(true);
    ASSERT_TRUE(outputs.veuRead.valid);
    EXPECT_EQ(outputs.veuRead.transactionId, 1u);
    EXPECT_EQ(outputs.veuRead.readData[0], 0x11);
    outputs = model.clock(true);
    ASSERT_TRUE(outputs.veuRead.valid);
    EXPECT_EQ(outputs.veuRead.transactionId, 2u);
    EXPECT_EQ(outputs.veuRead.readData[0], 0x22);
}

TEST(DutKuiMemoryModelTest, PreservesReadWhenWriteCompletesOnSameCycle)
{
    DutKuiMemoryModel model;
    model.writeByte(0x29120020, 0x5a);

    ASSERT_TRUE(model.acceptVeu({1, 0x29120020, false, 0, {}}));
    model.clock(true);
    model.clock(true);
    model.clock(true);

    DutKuiVeuRequest write;
    write.transactionId = 2;
    write.address = 0x29120420;
    write.isWrite = true;
    write.writeStrobe = 1;
    write.data[0] = 0xa5;
    ASSERT_TRUE(model.acceptVeu(write));

    const auto outputs = model.clock(true);
    ASSERT_TRUE(outputs.veuRead.valid);
    EXPECT_EQ(outputs.veuRead.transactionId, 1u);
    EXPECT_EQ(outputs.veuRead.readData[0], 0x5a);
    ASSERT_TRUE(outputs.veuWrite.valid);
    EXPECT_EQ(outputs.veuWrite.transactionId, 2u);
    EXPECT_EQ(model.readByte(0x29120420), 0xa5);
}

TEST(DutKuiMemoryModelTest, LimitsVeuOutstandingAndBlocksDbusDuringLock)
{
    DutKuiMemoryModel model;
    for (uint64_t id = 1; id <= 3; ++id) {
        ASSERT_TRUE(model.acceptVeu({id, 0x29120000, false, 0, {}}));
        model.clock(true);
    }
    ASSERT_TRUE(model.acceptVeu({4, 0x29120000, false, 0, {}}));
    EXPECT_EQ(model.veuOutstandingCount(), 4u);
    EXPECT_FALSE(model.acceptVeu({5, 0x29120000, false, 0, {}}));
    EXPECT_FALSE(model.acceptDbus({0x29120000, 0, 0}, true));

    ASSERT_TRUE(model.clock(true).veuRead.valid);
    EXPECT_EQ(model.veuOutstandingCount(), 3u);
    EXPECT_TRUE(model.acceptDbus({0x29120000, 0, 0}, false));
}

} // namespace
} // namespace brs
} // namespace gem5
