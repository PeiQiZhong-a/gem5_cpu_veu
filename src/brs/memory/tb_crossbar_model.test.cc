#include "brs/memory/tb_crossbar_model.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

TbBusRequest
readRequest(uint32_t address)
{
    TbBusRequest request;
    request.valid = true;
    request.address = address;
    request.read = true;
    return request;
}

TEST(TbCrossbarModelTest, ArbitratesVeuBeforeCapturedIbusAndDbus)
{
    TbCrossbarModel model;
    TbCrossbarInputs in;
    in.ibus = readRequest(0x00000000);
    in.dbus = readRequest(0x20010000);
    in.veu = readRequest(0x20010010);

    EXPECT_EQ(model.clock(in).grantedMaster, TbBusMaster::Veu);

    in = {};
    EXPECT_EQ(model.clock(in).grantedMaster, TbBusMaster::None);
    EXPECT_EQ(model.clock({}).grantedMaster, TbBusMaster::IBus);

    EXPECT_EQ(model.clock({}).grantedMaster, TbBusMaster::None);
    EXPECT_EQ(model.clock({}).grantedMaster, TbBusMaster::DBus);
}

TEST(TbCrossbarModelTest, LockAllowsOnlyVeuAndRetainsCpuPendingRequests)
{
    TbCrossbarModel model;
    TbCrossbarInputs in;
    in.lockStart = true;
    model.clock(in);

    in = {};
    in.dbus = readRequest(0x20010000);
    in.veu = readRequest(0x20010010);
    auto out = model.clock(in);
    EXPECT_TRUE(out.lockActive);
    EXPECT_EQ(out.grantedMaster, TbBusMaster::Veu);

    in = {};
    in.lockFinish = true;
    model.clock(in);
    EXPECT_EQ(model.clock({}).grantedMaster, TbBusMaster::DBus);
}

TEST(TbCrossbarModelTest, AppliesSynchronousSramAndConfiguredResponseDelay)
{
    TbCrossbarModel model;
    model.writeWord(0x20010000, 0x11223344);

    TbCrossbarInputs in;
    in.dbus = readRequest(0x20010000);
    auto out = model.clock(in);
    EXPECT_EQ(out.grantedMaster, TbBusMaster::None);
    EXPECT_FALSE(out.dbus.valid);

    out = model.clock({});
    EXPECT_EQ(out.grantedMaster, TbBusMaster::DBus);
    EXPECT_FALSE(model.clock({}).dbus.valid);
    EXPECT_FALSE(model.clock({}).dbus.valid);
    out = model.clock({});
    ASSERT_TRUE(out.dbus.valid);
    EXPECT_EQ(out.dbus.readData[0], 0x11223344u);
}

TEST(TbCrossbarModelTest, BusyClearsBeforeTheResponsePipelineCompletes)
{
    TbCrossbarModel model;
    TbCrossbarInputs in;
    in.ibus = readRequest(0x00000000);
    in.dbus = readRequest(0x20010000);

    EXPECT_EQ(model.clock(in).grantedMaster, TbBusMaster::None);
    EXPECT_EQ(model.clock({}).grantedMaster, TbBusMaster::IBus);
    EXPECT_EQ(model.clock({}).grantedMaster, TbBusMaster::None);
    EXPECT_EQ(model.clock({}).grantedMaster, TbBusMaster::DBus);
}

TEST(TbCrossbarModelTest, CpuWriteGetsDelayedCompletion)
{
    TbCrossbarModel model;
    TbCrossbarInputs in;
    in.dbus.valid = true;
    in.dbus.address = 0x20010004;
    in.dbus.write = true;
    in.dbus.writeStrobe = 0x00f0;
    in.dbus.writeData[1] = 0xaabbccdd;

    EXPECT_EQ(model.clock(in).grantedMaster, TbBusMaster::None);
    EXPECT_EQ(model.clock({}).grantedMaster, TbBusMaster::DBus);
    EXPECT_EQ(model.readWord(0x20010004), 0xaabbccddu);
    EXPECT_FALSE(model.clock({}).dbus.valid);
    EXPECT_FALSE(model.clock({}).dbus.valid);
    EXPECT_TRUE(model.clock({}).dbus.valid);
}

TEST(TbCrossbarModelTest, ModelsUartReadyRegister)
{
    TbCrossbarModel model;
    TbCrossbarInputs in;
    in.dbus = readRequest(0x40000008);
    model.clock(in);
    model.clock({});
    model.clock({});
    model.clock({});
    const auto out = model.clock({});
    ASSERT_TRUE(out.dbus.valid);
    EXPECT_EQ(out.dbus.readData[2], 1u);
}

TEST(TbCrossbarModelTest, ModelsTheFull128KiBPhysicalDataSram)
{
    TbCrossbarModel model;
    model.writeWord(0x20028000, 0xdeadbeef);

    TbCrossbarInputs in;
    in.dbus = readRequest(0x20028000);
    model.clock(in);
    model.clock({});
    model.clock({});
    model.clock({});
    const auto out = model.clock({});
    ASSERT_TRUE(out.dbus.valid);
    EXPECT_EQ(out.dbus.readData[0], 0xdeadbeefu);
}

TEST(TbCrossbarModelTest, VeuReadDataPinsRetainTheLastCompletedValue)
{
    TbCrossbarModel model;
    model.writeWord(0x20010000, 0x12345678);

    TbCrossbarInputs in;
    in.veu = readRequest(0x20010000);
    model.clock(in);

    TbCrossbarOutputs out;
    do {
        out = model.clock({});
    } while (!out.veu.valid);
    ASSERT_EQ(out.veu.readData[0], 0x12345678u);

    out = model.clock({});
    EXPECT_FALSE(out.veu.valid);
    EXPECT_EQ(out.veu.readData[0], 0x12345678u);
}

TEST(TbCrossbarModelTest, Honors128BitByteWriteStrobes)
{
    TbCrossbarModel model;
    TbCrossbarInputs in;
    in.veu.valid = true;
    in.veu.address = 0x20010000;
    in.veu.write = true;
    in.veu.writeStrobe = 0x00f3;
    in.veu.writeData = {
        0x44332211, 0x88776655, 0xccbbaa99, 0x00ffeedd};

    EXPECT_EQ(model.clock(in).grantedMaster, TbBusMaster::Veu);
    EXPECT_EQ(model.readByte(0x20010000), 0x11);
    EXPECT_EQ(model.readByte(0x20010001), 0x22);
    EXPECT_EQ(model.readByte(0x20010002), 0x00);
    EXPECT_EQ(model.readByte(0x20010003), 0x00);
    EXPECT_EQ(model.readWord(0x20010004), 0x88776655u);
}

} // namespace
} // namespace brs
} // namespace gem5
