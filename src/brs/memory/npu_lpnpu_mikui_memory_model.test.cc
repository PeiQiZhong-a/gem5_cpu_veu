#include "brs/memory/npu_lpnpu_mikui_memory_model.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

TEST(NpuLpnpuMikuiMemoryModelTest, UsesIndependentRegisteredIbus)
{
    NpuLpnpuMikuiMemoryModel model;
    model.writeWord(0x0, 0x12345678);
    model.writeWord(0xc, 0xaabbccdd);
    ASSERT_TRUE(model.acceptIbus({0x6}));

    const auto outputs = model.clock(false);
    ASSERT_TRUE(outputs.ibus.valid);
    EXPECT_EQ(outputs.ibus.readData[0], 0x12345678u);
    EXPECT_EQ(outputs.ibus.readData[3], 0xaabbccddu);
    EXPECT_FALSE(model.evaluate().dbus.valid);
}

TEST(NpuLpnpuMikuiMemoryModelTest, DbusTraversesExact32To128AndRvActive)
{
    NpuLpnpuMikuiMemoryModel model;
    constexpr uint32_t address = 0x2001000c;
    ASSERT_TRUE(model.acceptDbus({address, 0xf, 0x44332211}, false));

    DutKuiMemoryOutputs outputs;
    unsigned writeCycles = 0;
    do {
        outputs = model.clock(false);
        ++writeCycles;
    } while (!outputs.dbus.valid && writeCycles < 16);
    ASSERT_TRUE(outputs.dbus.valid);
    EXPECT_TRUE(outputs.dbus.isWrite);
    EXPECT_EQ(model.readWord(address), 0x44332211u);
    EXPECT_EQ(model.crossbarState(),
              NpuLpnpuMikuiCrossbar::State::RvActive);
    EXPECT_EQ(writeCycles, 7u);

    ASSERT_TRUE(model.acceptDbus({address, 0, 0}, false));
    unsigned readCycles = 0;
    do {
        outputs = model.clock(false);
        ++readCycles;
    } while (!outputs.dbus.valid && readCycles < 16);
    ASSERT_TRUE(outputs.dbus.valid);
    EXPECT_FALSE(outputs.dbus.isWrite);
    EXPECT_EQ(outputs.dbus.readData, 0x44332211u);
}

TEST(NpuLpnpuMikuiMemoryModelTest, SauUsesNative128BitBankPort)
{
    NpuLpnpuMikuiMemoryModel model;
    SauMemoryOutput sau;
    sau.crossbarStart = true;
    model.clock(false, sau);

    sau = {};
    sau.request.valid = true;
    sau.request.address = 0x20020000;
    sau.request.writeStrobe = uint16_t{1} << 15;
    sau.request.writeData[15] = 0x7e;
    auto outputs = model.clock(false, sau);
    EXPECT_TRUE(outputs.masterAccepted[
        static_cast<uint8_t>(NpuLpnpuMikuiMaster::Sau)]);
    EXPECT_EQ(model.readByte(0x2002000f), 0);

    outputs = model.clock(false);
    EXPECT_EQ(model.readByte(0x2002000f), 0x7e);
    outputs = model.clock(false);
    EXPECT_TRUE(outputs.sau.valid);
}

TEST(NpuLpnpuMikuiMemoryModelTest, UsesNative128BitVeuBeat)
{
    NpuLpnpuMikuiMemoryModel model;
    DutKuiVeuRequest request;
    request.transactionId = 9;
    request.address = 0x20010020;
    request.isWrite = true;
    request.writeStrobe = VeuFullWriteMask;
    request.data[0] = 0x12;
    request.data[15] = 0x34;
    ASSERT_TRUE(model.acceptVeu(request));

    DutKuiMemoryOutputs outputs;
    for (unsigned cycle = 0; cycle < 12 && !outputs.veuWrite.valid;
         ++cycle) {
        outputs = model.clock(true);
    }
    ASSERT_TRUE(outputs.veuWrite.valid);
    EXPECT_EQ(outputs.veuWrite.transactionId, 9u);
    EXPECT_EQ(model.readByte(0x20010020), 0x12);
    EXPECT_EQ(model.readByte(0x2001002f), 0x34);
    EXPECT_EQ(model.veuOutstandingCount(), 0u);
}

TEST(NpuLpnpuMikuiMemoryModelTest, KeepsBothPhysicalBanksDistinct)
{
    NpuLpnpuMikuiMemoryModel model;
    model.writeByte(0x20010000, 0x11);
    model.writeByte(0x20020000, 0x22);
    EXPECT_EQ(model.readByte(0x20010000), 0x11);
    EXPECT_EQ(model.readByte(0x20020000), 0x22);
    EXPECT_EQ(model.readByte(0x20000000), 0);
}

TEST(NpuLpnpuMikuiMemoryModelTest, DmaTopologyUsesThreePhysicalBanks)
{
    NpuLpnpuMikuiMemoryModel::Config config;
    config.dmaTopology = true;
    NpuLpnpuMikuiMemoryModel model(config);
    model.writeByte(0x20010000, 0x11);
    model.writeByte(0x20018000, 0x22);
    model.writeByte(0x20020000, 0x33);
    EXPECT_EQ(model.readByte(0x20010000), 0x11);
    EXPECT_EQ(model.readByte(0x20018000), 0x22);
    EXPECT_EQ(model.readByte(0x20020000), 0x33);
    EXPECT_EQ(model.readByte(0x20028000), 0);
}

} // namespace
} // namespace brs
} // namespace gem5
