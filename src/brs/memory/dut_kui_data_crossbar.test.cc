#include "brs/memory/dut_kui_data_crossbar.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

Sram256Request
readRequest(uint32_t address)
{
    Sram256Request request;
    request.valid = true;
    request.address = address;
    return request;
}

TEST(DutKuiDataCrossbarTest, FollowsIdleActiveRvActiveTransitions)
{
    DutKuiDataCrossbar crossbar;
    DutKuiDataCrossbarInputs inputs;
    EXPECT_EQ(crossbar.state(), DutKuiDataCrossbar::State::Idle);

    inputs.dbus = readRequest(0x29120000);
    crossbar.clock(inputs);
    EXPECT_EQ(crossbar.state(), DutKuiDataCrossbar::State::RvActive);

    inputs.crossbarStart[0] = true;
    crossbar.clock(inputs);
    EXPECT_EQ(crossbar.state(), DutKuiDataCrossbar::State::Active);

    inputs = {};
    inputs.crossbarDone[1] = true;
    crossbar.clock(inputs);
    EXPECT_EQ(crossbar.state(), DutKuiDataCrossbar::State::Idle);
}

TEST(DutKuiDataCrossbarTest, SauAndVeuRunInParallelOnDifferentBanks)
{
    DutKuiDataCrossbar crossbar;
    DutKuiDataCrossbarInputs inputs;
    inputs.crossbarStart[0] = true;
    crossbar.clock(inputs);

    inputs = {};
    inputs.masters[0] = readRequest(0x29120000);
    inputs.masters[1] = readRequest(0x29130000);
    crossbar.clock(inputs);
    const auto output = crossbar.evaluate();
    EXPECT_TRUE(output.acceptedMaster[0]);
    EXPECT_TRUE(output.acceptedMaster[1]);
    EXPECT_TRUE(output.bankRequest[0]);
    EXPECT_TRUE(output.bankRequest[1]);
}

TEST(DutKuiDataCrossbarTest, StartOpensOnlyTheFollowingActiveCycle)
{
    DutKuiDataCrossbar crossbar;
    DutKuiDataCrossbarInputs inputs;
    inputs.crossbarStart[0] = true;
    inputs.masters[0] = readRequest(0x29120000);
    crossbar.clock(inputs);

    EXPECT_EQ(crossbar.state(), DutKuiDataCrossbar::State::Active);
    EXPECT_FALSE(crossbar.evaluate().acceptedMaster[0]);
    EXPECT_FALSE(crossbar.evaluate().bankRequest[0]);

    inputs.crossbarStart[0] = false;
    crossbar.clock(inputs);
    EXPECT_TRUE(crossbar.evaluate().acceptedMaster[0]);

    inputs = {};
    inputs.crossbarDone[0] = true;
    crossbar.clock(inputs);
    EXPECT_EQ(crossbar.state(), DutKuiDataCrossbar::State::Idle);
}

TEST(DutKuiDataCrossbarTest, VeuWinsSameBankCollisionLikeRtlLoopOrder)
{
    DutKuiDataCrossbar crossbar;
    DutKuiDataCrossbarInputs inputs;
    inputs.crossbarStart[0] = true;
    crossbar.clock(inputs);

    inputs = {};
    inputs.masters[0] = readRequest(0x29120000);
    inputs.masters[1] = readRequest(0x29120020);
    crossbar.clock(inputs);
    const auto output = crossbar.evaluate();
    EXPECT_FALSE(output.acceptedMaster[0]);
    EXPECT_TRUE(output.acceptedMaster[1]);
    EXPECT_TRUE(output.droppedMaster[0]);
    EXPECT_FALSE(output.droppedMaster[1]);
    EXPECT_TRUE(output.sameBankCollision);
    EXPECT_TRUE(output.bankRequest[0]);
}

TEST(DutKuiDataCrossbarTest, ConsecutiveValidCyclesAreDistinctSramBeats)
{
    DutKuiDataCrossbar crossbar;
    DutKuiDataCrossbarInputs inputs;
    inputs.crossbarStart[0] = true;
    crossbar.clock(inputs);

    inputs = {};
    inputs.masters[0] = readRequest(0x29120000);
    crossbar.clock(inputs);
    EXPECT_TRUE(crossbar.evaluate().acceptedMaster[0]);

    inputs.masters[0].address = 0x29120020;
    crossbar.clock(inputs);
    EXPECT_TRUE(crossbar.evaluate().acceptedMaster[0]);

    crossbar.clock({});
    EXPECT_TRUE(crossbar.evaluate().masters[0].valid);
    crossbar.clock({});
    EXPECT_TRUE(crossbar.evaluate().masters[0].valid);
}

TEST(DutKuiDataCrossbarTest, ThreeRealBanksStoreAndFourthPortIsDummy)
{
    DutKuiDataCrossbar crossbar;
    crossbar.writeWord(0x29120000, 0x11111111);
    crossbar.writeWord(0x29130000, 0x22222222);
    crossbar.writeWord(0x29140000, 0x33333333);
    crossbar.writeWord(0x29150000, 0x44444444);

    EXPECT_EQ(crossbar.readWord(0x29120000), 0x11111111);
    EXPECT_EQ(crossbar.readWord(0x29130000), 0x22222222);
    EXPECT_EQ(crossbar.readWord(0x29140000), 0x33333333);
    EXPECT_EQ(crossbar.readWord(0x29150000), 0);
    EXPECT_TRUE(crossbar.isRealBank(2));
    EXPECT_FALSE(crossbar.isRealBank(3));
}

TEST(DutKuiDataCrossbarTest, MasterReadReturnsAfterTwoRegisteredEdges)
{
    DutKuiDataCrossbar crossbar;
    crossbar.writeByte(0x29120000, 0x5a);
    DutKuiDataCrossbarInputs inputs;
    inputs.crossbarStart[0] = true;
    crossbar.clock(inputs);

    inputs = {};
    inputs.masters[0] = readRequest(0x29120000);
    crossbar.clock(inputs);
    EXPECT_FALSE(crossbar.evaluate().masters[0].valid);
    crossbar.clock({});
    EXPECT_FALSE(crossbar.evaluate().masters[0].valid);
    crossbar.clock({});
    const auto output = crossbar.evaluate();
    ASSERT_TRUE(output.masters[0].valid);
    EXPECT_EQ(output.masters[0].readData[0], 0x5a);
}

TEST(DutKuiDataCrossbarTest, UnmappedDbusGetsResponseWithoutBankRequest)
{
    DutKuiDataCrossbar crossbar;
    DutKuiDataCrossbarInputs inputs;
    inputs.dbus = readRequest(0xdead0000);
    crossbar.clock(inputs);
    crossbar.clock(inputs);
    const auto output = crossbar.evaluate();
    EXPECT_TRUE(output.dbus.valid);
    EXPECT_TRUE(output.acceptedDbus);
    for (bool requested : output.bankRequest) {
        EXPECT_FALSE(requested);
    }
}

} // namespace
} // namespace brs
} // namespace gem5
