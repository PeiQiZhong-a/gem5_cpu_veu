#include "brs/memory/dut_kui_memory_model.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

TEST(DutKuiMemoryModelTest, EvaluateIsStableUntilClockEdge)
{
    DutKuiMemoryModel model;
    model.writeWord(0, 0x12345678);
    ASSERT_TRUE(model.acceptIbus({0}));

    const DutKuiMemoryOutputs before = model.evaluate();
    EXPECT_FALSE(before.ibus.valid);
    EXPECT_FALSE(model.evaluate().ibus.valid);

    model.clockEdge(false);
    EXPECT_FALSE(model.evaluate().ibus.valid);
    EXPECT_FALSE(model.evaluate().ibus.valid);

    model.clockEdge(false);
    ASSERT_TRUE(model.evaluate().ibus.valid);
    EXPECT_EQ(model.evaluate().ibus.readData[0], 0x12345678u);
}

TEST(DutKuiMemoryModelTest, KeepsIbusOnIndependent128BitPath)
{
    DutKuiMemoryModel model;
    for (uint32_t word = 0; word < 4; ++word) {
        model.writeWord(word * 4, 0x100u + word);
    }
    ASSERT_TRUE(model.acceptIbus({0x6u}));
    EXPECT_FALSE(model.acceptIbus({0x10u}));

    DutKuiMemoryOutputs outputs = model.clock(false);
    EXPECT_FALSE(outputs.ibus.valid);
    outputs = model.clock(false);
    ASSERT_TRUE(outputs.ibus.valid);
    EXPECT_EQ(outputs.ibus.readData[0], 0x100u);
    EXPECT_EQ(outputs.ibus.readData[3], 0x103u);
}

TEST(DutKuiMemoryModelTest, DbusTraversesRegistered32To256AndRvActivePath)
{
    DutKuiMemoryModel model;
    constexpr uint32_t address = 0x29120014;
    ASSERT_TRUE(model.acceptDbus({address, 0xf, 0xaabbccdd}, false));

    auto outputs = model.clock(false);
    EXPECT_EQ(model.converterState(), SramConverter32To256::State::WaitAck);
    EXPECT_EQ(model.crossbarState(), DutKuiDataCrossbar::State::Idle);
    EXPECT_FALSE(outputs.dbus.valid);

    outputs = model.clock(false);
    EXPECT_EQ(model.crossbarState(), DutKuiDataCrossbar::State::RvActive);
    EXPECT_EQ(model.readWord(address), 0);

    outputs = model.clock(false);
    EXPECT_EQ(model.readWord(address), 0xaabbccddu);
    EXPECT_FALSE(outputs.dbus.valid);

    EXPECT_FALSE(model.clock(false).dbus.valid);
    EXPECT_FALSE(model.clock(false).dbus.valid);
    outputs = model.clock(false);
    ASSERT_TRUE(outputs.dbus.valid);
    EXPECT_TRUE(outputs.dbus.isWrite);
    EXPECT_EQ(model.converterState(), SramConverter32To256::State::Idle);

    ASSERT_TRUE(model.acceptDbus({address, 0, 0}, false));
    for (int cycle = 0; cycle < 5; ++cycle) {
        EXPECT_FALSE(model.clock(false).dbus.valid) << "cycle=" << cycle;
    }
    outputs = model.clock(false);
    ASSERT_TRUE(outputs.dbus.valid);
    EXPECT_FALSE(outputs.dbus.isWrite);
    EXPECT_EQ(outputs.dbus.readData, 0xaabbccddu);
}

TEST(DutKuiMemoryModelTest, AdaptsMikuiVeuBeatTo256BitMasterPath)
{
    DutKuiMemoryModel model;
    DutKuiVeuRequest request;
    request.transactionId = 7;
    request.address = 0x29120010;
    request.isWrite = true;
    request.writeStrobe = (uint32_t{1} << 0) | (uint32_t{1} << 15);
    request.data[0] = 0x12;
    request.data[15] = 0xfe;
    ASSERT_TRUE(model.acceptVeu(request));

    EXPECT_FALSE(model.clock(true).veuWrite.valid);
    EXPECT_EQ(model.crossbarState(), DutKuiDataCrossbar::State::Active);
    EXPECT_FALSE(model.clock(true).veuWrite.valid);
    EXPECT_EQ(model.readByte(0x29120010), 0x12);
    EXPECT_EQ(model.readByte(0x2912001f), 0xfe);
    EXPECT_EQ(model.readByte(0x29120011), 0);
    EXPECT_FALSE(model.clock(true).veuWrite.valid);
    const auto outputs = model.clock(true);
    ASSERT_TRUE(outputs.veuWrite.valid);
    EXPECT_TRUE(outputs.veuWrite.isWrite);
    EXPECT_EQ(outputs.veuWrite.transactionId, 7u);
}

TEST(DutKuiMemoryModelTest, ReturnsPipelinedVeuReadsInIssueOrder)
{
    DutKuiMemoryModel model;
    model.writeByte(0x29120020, 0x11);
    model.writeByte(0x29120040, 0x22);

    ASSERT_TRUE(model.acceptVeu({1, 0x29120020, false, 0, {}}));
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

TEST(DutKuiMemoryModelTest, SauAndVeuShareTheSameThreeRealBanks)
{
    DutKuiMemoryModel model;
    DutKuiVeuRequest veu;
    veu.transactionId = 1;
    veu.address = 0x29130020;
    veu.isWrite = true;
    veu.writeStrobe = 1;
    veu.data[0] = 0x5a;
    ASSERT_TRUE(model.acceptVeu(veu));

    SauMemoryOutput sau;
    sau.crossbarStart = true;
    model.clock(true, sau);

    sau.crossbarStart = false;
    sau.request.valid = true;
    sau.request.address = 0x29120020;
    sau.request.writeStrobe = 1;
    sau.request.writeData[0] = 0xa5;
    model.clock(true, sau);

    EXPECT_EQ(model.readByte(0x29120020), 0xa5);
    EXPECT_EQ(model.readByte(0x29130020), 0x5a);

    // The fourth decoded dut_kui port is tied to the wrapper dummy.
    model.writeByte(0x29150000, 0xff);
    EXPECT_EQ(model.readByte(0x29150000), 0);
}

TEST(DutKuiMemoryModelTest, AdaptsSau128BitBeatsToLegacy256BitHalves)
{
    DutKuiMemoryModel::Config config;
    config.crossbarMasterResponseLatency = 0;
    DutKuiMemoryModel model(config);

    SauMemoryOutput sau;
    sau.crossbarStart = true;
    model.clock(false, sau);

    // A lower-half SAU beat maps byte 0 to the 256-bit line's byte 0.
    sau = {};
    sau.request.valid = true;
    sau.request.address = 0x29120020;
    sau.request.writeStrobe = uint16_t{1} << 0;
    sau.request.writeData[0] = 0x12;
    auto outputs = model.clock(false, sau);
    EXPECT_TRUE(outputs.masterAccepted[
        static_cast<uint8_t>(DutKuiDataMaster::Sau)]);
    EXPECT_EQ(model.readByte(0x29120020), 0x12);

    // Address bit 4 selects the upper half: canonical byte 0 becomes the
    // legacy 256-bit line's byte 16, without changing SAU byte ordering.
    sau.request.address = 0x29120030;
    sau.request.writeData[0] = 0x34;
    outputs = model.clock(false, sau);
    EXPECT_TRUE(outputs.sau.valid); // completion of the preceding write
    EXPECT_TRUE(outputs.masterAccepted[
        static_cast<uint8_t>(DutKuiDataMaster::Sau)]);
    EXPECT_EQ(model.readByte(0x29120030), 0x34);
    EXPECT_EQ(model.readByte(0x29120020), 0x12);

    // Drain the upper-half write response, then prove read data is sliced
    // back down to canonical bytes 0..15.
    EXPECT_TRUE(model.clock(false).sau.valid);
    model.writeByte(0x2912003f, 0x7e);
    sau = {};
    sau.request.valid = true;
    sau.request.address = 0x29120030;
    model.clock(false, sau);
    outputs = model.clock(false);
    ASSERT_TRUE(outputs.sau.valid);
    EXPECT_EQ(outputs.sau.readData[0], 0x34);
    EXPECT_EQ(outputs.sau.readData[15], 0x7e);
}

TEST(DutKuiMemoryModelTest, ReportsRtlSameBankWinnerAndDroppedSauBeat)
{
    DutKuiMemoryModel model;
    DutKuiVeuRequest veu;
    veu.transactionId = 1;
    veu.address = 0x29120020;
    veu.isWrite = true;
    veu.writeStrobe = 1;
    veu.data[0] = 0x5a;
    ASSERT_TRUE(model.acceptVeu(veu));

    SauMemoryOutput sau;
    sau.crossbarStart = true;
    model.clock(true, sau);

    sau.crossbarStart = false;
    sau.request.valid = true;
    sau.request.address = 0x29120040;
    sau.request.writeStrobe = 1;
    sau.request.writeData[0] = 0xa5;
    const DutKuiMemoryOutputs outputs = model.clock(true, sau);

    EXPECT_TRUE(outputs.sameBankCollision);
    EXPECT_FALSE(outputs.masterAccepted[
        static_cast<uint8_t>(DutKuiDataMaster::Sau)]);
    EXPECT_TRUE(outputs.masterDropped[
        static_cast<uint8_t>(DutKuiDataMaster::Sau)]);
    EXPECT_TRUE(outputs.masterAccepted[
        static_cast<uint8_t>(DutKuiDataMaster::Veu)]);
    EXPECT_EQ(model.readByte(0x29120020), 0x5a);
    EXPECT_EQ(model.readByte(0x29120040), 0);
}

TEST(DutKuiMemoryModelTest, LimitsVeuOutstandingButQueuesDbusDuringLock)
{
    DutKuiMemoryModel::Config config;
    config.crossbarMasterResponseLatency = 8;
    DutKuiMemoryModel model(config);
    for (uint64_t id = 1; id <= 4; ++id) {
        ASSERT_TRUE(model.acceptVeu({id, 0x29120000, false, 0, {}}));
        model.clock(true);
    }
    EXPECT_EQ(model.veuOutstandingCount(), 4u);
    EXPECT_FALSE(model.acceptVeu({5, 0x29120000, false, 0, {}}));

    // The RTL converter may capture DBUS while the crossbar is ACTIVE; the
    // request waits until the subcore lock ends instead of being rejected.
    EXPECT_TRUE(model.acceptDbus({0x29120000, 0, 0}, true));
    EXPECT_TRUE(model.dbusHasOutstanding());
    for (int cycle = 0; cycle < 4; ++cycle) {
        EXPECT_FALSE(model.clock(true).dbus.valid);
    }

    DutKuiMemoryOutputs outputs;
    for (int cycle = 0; cycle < 12 && !outputs.dbus.valid; ++cycle) {
        outputs = model.clock(false);
    }
    EXPECT_TRUE(outputs.dbus.valid);
}

} // namespace
} // namespace brs
} // namespace gem5
