#include "brs/memory/npu_lpnpu_mikui_crossbar.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

Sram128Request
writeRequest(uint32_t address, uint8_t value)
{
    Sram128Request request;
    request.valid = true;
    request.address = address;
    request.writeStrobe = 1;
    request.writeData[0] = value;
    return request;
}

TEST(NpuLpnpuMikuiCrossbarTest, UsesRtlAddressMap)
{
    NpuLpnpuMikuiCrossbar crossbar;
    EXPECT_EQ(crossbar.decodeMasterBank(0x20010000), 0);
    EXPECT_EQ(crossbar.decodeMasterBank(0x2001ffff), 0);
    EXPECT_EQ(crossbar.decodeMasterBank(0x20020000), 1);
    EXPECT_EQ(crossbar.decodeMasterBank(0x2002ffff), 1);
    EXPECT_EQ(crossbar.decodeMasterBank(0x20000000), -1);
    EXPECT_EQ(crossbar.decodeMasterBank(0x20030000), -1);

    EXPECT_EQ(crossbar.decodeDbusBank(0x20010000), 0);
    EXPECT_EQ(crossbar.decodeDbusBank(0x2001ffff), 0);
    EXPECT_EQ(crossbar.decodeDbusBank(0x20000000), 1);
    EXPECT_EQ(crossbar.decodeDbusBank(0x2fffffff), 1);
}

TEST(NpuLpnpuMikuiCrossbarTest, StartHasPriorityAndFirstBeatIsLater)
{
    NpuLpnpuMikuiCrossbar crossbar;
    crossbar.reset();

    NpuLpnpuMikuiCrossbarInputs inputs;
    inputs.dbus = writeRequest(0x20010000, 0x11);
    inputs.crossbarStart[0] = true;
    inputs.masters[0] = writeRequest(0x20010000, 0x22);
    crossbar.clock(inputs);

    EXPECT_EQ(crossbar.state(), NpuLpnpuMikuiCrossbar::State::Active);
    EXPECT_FALSE(crossbar.evaluate().bankRequest[0]);
    EXPECT_EQ(crossbar.readByte(0x20010000), 0);

    inputs = {};
    inputs.masters[0] = writeRequest(0x20010000, 0x33);
    crossbar.clock(inputs);
    EXPECT_TRUE(crossbar.evaluate().acceptedMaster[0]);
    EXPECT_TRUE(crossbar.evaluate().bankRequest[0]);
    EXPECT_EQ(crossbar.readByte(0x20010000), 0);

    // The registered bank request is sampled by SRAM on the next edge.
    crossbar.clock({});
    EXPECT_EQ(crossbar.readByte(0x20010000), 0x33);
}

TEST(NpuLpnpuMikuiCrossbarTest, RvActiveIsStickyLikeRtl)
{
    NpuLpnpuMikuiCrossbar crossbar;
    crossbar.reset();
    NpuLpnpuMikuiCrossbarInputs inputs;
    inputs.dbus.valid = true;
    inputs.dbus.address = 0x20010000;
    crossbar.clock(inputs);
    EXPECT_EQ(crossbar.state(), NpuLpnpuMikuiCrossbar::State::RvActive);

    crossbar.clock({});
    EXPECT_EQ(crossbar.state(), NpuLpnpuMikuiCrossbar::State::RvActive);
    crossbar.clock({});
    EXPECT_EQ(crossbar.state(), NpuLpnpuMikuiCrossbar::State::RvActive);

    inputs = {};
    inputs.crossbarStart[1] = true;
    crossbar.clock(inputs);
    EXPECT_EQ(crossbar.state(), NpuLpnpuMikuiCrossbar::State::Active);
}

TEST(NpuLpnpuMikuiCrossbarTest, DbusResponseUsesRegisteredAck)
{
    NpuLpnpuMikuiCrossbar crossbar;
    crossbar.reset();
    crossbar.writeByte(0x20010004, 0x78);
    crossbar.writeByte(0x20010005, 0x56);
    crossbar.writeByte(0x20010006, 0x34);
    crossbar.writeByte(0x20010007, 0x12);

    NpuLpnpuMikuiCrossbarInputs inputs;
    inputs.dbus.valid = true;
    inputs.dbus.address = 0x20010000;
    crossbar.clock(inputs); // IDLE -> RVACTIVE
    EXPECT_FALSE(crossbar.evaluate().dbus.valid);
    crossbar.clock(inputs); // register slave_req
    EXPECT_FALSE(crossbar.evaluate().dbus.valid);
    crossbar.clock(inputs); // SRAM data and slave_ack register
    EXPECT_FALSE(crossbar.evaluate().dbus.valid);
    crossbar.clock(inputs); // DBUS samples registered data/ack
    ASSERT_TRUE(crossbar.evaluate().dbus.valid);
    EXPECT_EQ(crossbar.evaluate().dbus.readData[4], 0x78);
    EXPECT_EQ(crossbar.evaluate().dbus.readData[7], 0x12);
}

TEST(NpuLpnpuMikuiCrossbarTest, VeuWinsSameBankCollision)
{
    NpuLpnpuMikuiCrossbar crossbar;
    crossbar.reset();
    NpuLpnpuMikuiCrossbarInputs inputs;
    inputs.crossbarStart[0] = true;
    crossbar.clock(inputs);

    inputs = {};
    inputs.masters[0] = writeRequest(0x20010000, 0xa5);
    inputs.masters[1] = writeRequest(0x20010000, 0x5a);
    crossbar.clock(inputs);
    const auto arbitration = crossbar.evaluate();
    EXPECT_TRUE(arbitration.sameBankCollision);
    EXPECT_TRUE(arbitration.droppedMaster[0]);
    EXPECT_FALSE(arbitration.acceptedMaster[0]);
    EXPECT_TRUE(arbitration.acceptedMaster[1]);

    crossbar.clock({});
    EXPECT_EQ(crossbar.readByte(0x20010000), 0x5a);
    crossbar.clock({});
    // Both delayed selectors observe the winning bank data, matching the
    // internal RTL master_rdata behavior even though SAU lost the write.
    EXPECT_TRUE(crossbar.evaluate().masters[0].valid);
    EXPECT_TRUE(crossbar.evaluate().masters[1].valid);
    EXPECT_EQ(crossbar.evaluate().masters[0].readData[0], 0x5a);
    EXPECT_EQ(crossbar.evaluate().masters[1].readData[0], 0x5a);
}

TEST(NpuLpnpuMikuiCrossbarTest, DoneEdgeStillExecutesActiveLogic)
{
    NpuLpnpuMikuiCrossbar crossbar;
    crossbar.reset();
    NpuLpnpuMikuiCrossbarInputs inputs;
    inputs.crossbarStart[0] = true;
    crossbar.clock(inputs);

    inputs = {};
    inputs.crossbarDone[0] = true;
    inputs.masters[0] = writeRequest(0x20020000, 0x7e);
    crossbar.clock(inputs);
    EXPECT_EQ(crossbar.state(), NpuLpnpuMikuiCrossbar::State::Idle);
    EXPECT_TRUE(crossbar.evaluate().bankRequest[1]);
    EXPECT_EQ(crossbar.readByte(0x20020000), 0);

    crossbar.clock({});
    EXPECT_EQ(crossbar.readByte(0x20020000), 0x7e);
    EXPECT_FALSE(crossbar.evaluate().bankRequest[1]);
}

} // namespace
} // namespace brs
} // namespace gem5
