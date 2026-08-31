#include "brs/memory/npu_lpnpu_mikui_dma_crossbar.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

Sram128Request
dmaWriteRequest(uint32_t address, uint8_t value)
{
    Sram128Request request;
    request.valid = true;
    request.address = address;
    request.writeStrobe = 1;
    request.writeData[0] = value;
    return request;
}

TEST(NpuLpnpuMikuiDmaCrossbarTest, UsesDutMikuiDmaDefaultSplit)
{
    NpuLpnpuMikuiDmaCrossbar crossbar;
    EXPECT_EQ(crossbar.decodeBank(0x20010000), 0);
    EXPECT_EQ(crossbar.decodeBank(0x20017fff), 0);
    EXPECT_EQ(crossbar.decodeBank(0x20018000), 1);
    EXPECT_EQ(crossbar.decodeBank(0x2001ffff), 1);
    EXPECT_EQ(crossbar.decodeBank(0x20020000), 2);
    EXPECT_EQ(crossbar.decodeBank(0x20027fff), 2);
    EXPECT_EQ(crossbar.decodeBank(0x2000ffff), -1);
    EXPECT_EQ(crossbar.decodeBank(0x20028000), -1);
}

TEST(NpuLpnpuMikuiDmaCrossbarTest, KeepsStackPingPongDistinct)
{
    NpuLpnpuMikuiDmaCrossbar crossbar;
    crossbar.writeByte(0x20010000, 0x11);
    crossbar.writeByte(0x20018000, 0x22);
    crossbar.writeByte(0x20020000, 0x33);
    EXPECT_EQ(crossbar.readByte(0x20010000), 0x11);
    EXPECT_EQ(crossbar.readByte(0x20018000), 0x22);
    EXPECT_EQ(crossbar.readByte(0x20020000), 0x33);
}

TEST(NpuLpnpuMikuiDmaCrossbarTest, AllowsDifferentBanksInParallel)
{
    NpuLpnpuMikuiDmaCrossbar crossbar;
    NpuLpnpuMikuiCrossbarInputs inputs;
    inputs.crossbarStart[0] = true;
    inputs.crossbarStart[1] = true;
    crossbar.clock(inputs);

    inputs = {};
    inputs.masters[0] = dmaWriteRequest(0x20018000, 0xa5);
    inputs.masters[1] = dmaWriteRequest(0x20020000, 0x5a);
    crossbar.clock(inputs);
    const auto outputs = crossbar.evaluate();
    EXPECT_TRUE(outputs.acceptedMaster[0]);
    EXPECT_TRUE(outputs.acceptedMaster[1]);
    EXPECT_TRUE(outputs.masters[0].valid);
    EXPECT_TRUE(outputs.masters[1].valid);
    EXPECT_TRUE(outputs.bankRequest[1]);
    EXPECT_TRUE(outputs.bankRequest[2]);
    EXPECT_FALSE(outputs.sameBankCollision);
    EXPECT_EQ(crossbar.readByte(0x20018000), 0xa5);
    EXPECT_EQ(crossbar.readByte(0x20020000), 0x5a);
}

TEST(NpuLpnpuMikuiDmaCrossbarTest, LaterVeuAssignmentWinsCollision)
{
    NpuLpnpuMikuiDmaCrossbar crossbar;
    NpuLpnpuMikuiCrossbarInputs inputs;
    inputs.crossbarStart[0] = true;
    crossbar.clock(inputs);

    inputs = {};
    inputs.masters[0] = dmaWriteRequest(0x20010000, 0xa5);
    inputs.masters[1] = dmaWriteRequest(0x20010000, 0x5a);
    crossbar.clock(inputs);
    const auto outputs = crossbar.evaluate();
    EXPECT_TRUE(outputs.sameBankCollision);
    EXPECT_TRUE(outputs.droppedMaster[0]);
    EXPECT_TRUE(outputs.acceptedMaster[1]);
    EXPECT_TRUE(outputs.masters[0].valid);
    EXPECT_TRUE(outputs.masters[1].valid);
    EXPECT_EQ(crossbar.readByte(0x20010000), 0x5a);
}

TEST(NpuLpnpuMikuiDmaCrossbarTest, DbusDecodeHolesDefaultToStack)
{
    for (const uint32_t address : {0x20008000u, 0x2000ffffu,
                                   0x20028000u}) {
        NpuLpnpuMikuiDmaCrossbar crossbar;
        NpuLpnpuMikuiCrossbarInputs inputs;
        inputs.dbus = dmaWriteRequest(address, 0x6b);
        crossbar.clock(inputs);
        const auto outputs = crossbar.evaluate();
        EXPECT_TRUE(outputs.dbus.valid);
        EXPECT_TRUE(outputs.bankRequest[0]);
        EXPECT_FALSE(outputs.addressError);
    }
}

TEST(NpuLpnpuMikuiDmaCrossbarTest, DbusOutsideSplitFlagsErrorButDrivesStack)
{
    NpuLpnpuMikuiDmaCrossbar crossbar;
    NpuLpnpuMikuiCrossbarInputs inputs;
    inputs.dbus = dmaWriteRequest(0x20007ff0, 0x7c);
    crossbar.clock(inputs);
    const auto outputs = crossbar.evaluate();
    EXPECT_TRUE(outputs.dbus.valid);
    EXPECT_TRUE(outputs.bankRequest[0]);
    EXPECT_TRUE(outputs.addressError);
}

} // namespace
} // namespace brs
} // namespace gem5
