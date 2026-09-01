#include <gtest/gtest.h>

#include "sau_mikui/sau_address_generator.hh"
#include "sau_mikui/sau_feeder.hh"
#include "sau_mikui/sau_memory_controller.hh"
#include "sau_mikui/sau_register_file.hh"
#include "sau_mikui/sau_shift_register.hh"

namespace gem5::sau_mikui
{
namespace
{

TEST(SauAddressGeneratorTest, DelaysEveryAddressBySixCycles)
{
    SauAddressGenerator address;
    address.reset();

    SauCommand command;
    command.verticalBase = 0x10000;
    command.verticalXStep = 1;
    command.verticalChannelStep = 16;
    command.flowLoopTimes = 1;

    SauAddressInputs input;
    input.start = true;
    input.command = command;
    address.computeNext(input);
    address.commit();

    input = {};
    input.state = SchedulerState::FirstLoad;
    input.inputSwitch = 1;
    input.command = command;
    for (unsigned cycle = 0; cycle < SauConstants::AddressDelay; ++cycle) {
        EXPECT_FALSE(address.evaluate().readEnable) << cycle;
        address.computeNext(input);
        address.commit();
    }

    const auto output = address.evaluate();
    EXPECT_TRUE(output.readEnable);
    EXPECT_EQ(output.address, SauConstants::BaseAddress + 0x10000);
}

TEST(SauMemoryControllerTest, RegistersRequestsAndAlignsReadData)
{
    SauMemoryController memory;
    memory.reset();

    SauMemoryControllerInputs input;
    input.readEnable = true;
    input.readLast = true;
    input.address = 0x20010020;
    memory.computeNext(input);
    memory.commit();

    ASSERT_TRUE(memory.evaluate().request.valid);
    EXPECT_FALSE(memory.evaluate().request.isWrite());
    EXPECT_EQ(memory.evaluate().request.address, 0x20010020u);
    EXPECT_FALSE(memory.evaluate().readValid);

    input = {};
    memory.computeNext(input);
    memory.commit();
    EXPECT_FALSE(memory.evaluate().readValid);

    input = {};
    input.response.valid = true;
    input.response.readData[0] = 0x5a;
    memory.computeNext(input);
    memory.commit();
    EXPECT_FALSE(memory.evaluate().readValid);

    input = {};
    memory.computeNext(input);
    memory.commit();
    EXPECT_TRUE(memory.evaluate().readValid);
    EXPECT_TRUE(memory.evaluate().readLast);
    EXPECT_EQ(memory.evaluate().readData[0], 0x5a);
    EXPECT_FALSE(memory.timingError());
}

TEST(SauMemoryControllerTest, WritesFullBeatAndPulsesFinalDone)
{
    SauMemoryController memory;
    SauMemoryControllerInputs input;
    input.writeEnable = true;
    input.writeLast = true;
    input.lastInstruction = true;
    input.schedulerState = SchedulerState::DOut;
    input.address = 0x20020000;
    input.writeData[15] = 0xa5;

    memory.computeNext(input);
    memory.commit();
    const auto output = memory.evaluate();
    EXPECT_TRUE(output.request.valid);
    EXPECT_TRUE(output.request.isWrite());
    EXPECT_EQ(output.request.writeStrobe, 0xffff);
    EXPECT_EQ(output.request.writeData[15], 0xa5);
    EXPECT_TRUE(output.lastInstructionWriteDone);
}

TEST(SauMemoryControllerTest, CountsMissingAndEarlyResponsesInStrictMode)
{
    SauMemoryController memory;
    SauMemoryControllerInputs input;
    input.response.valid = true;
    memory.computeNext(input);
    memory.commit();
    EXPECT_EQ(memory.errors().earlyResponses, 1u);

    input = {};
    input.readEnable = true;
    memory.computeNext(input);
    memory.commit();
    input = {};
    memory.computeNext(input);
    memory.commit();
    EXPECT_EQ(memory.errors().missingResponses, 0u);
    memory.computeNext(input);
    memory.commit();
    EXPECT_EQ(memory.errors().missingResponses, 1u);
}

TEST(SauRegisterFileTest, StoresMaskedBeatAndStreamsKernelWindow)
{
    SauRegisterFile file;
    file.reset();
    SauRegisterFileInputs input;
    input.kernel = 3;
    input.writeValid = true;
    for (unsigned i = 0; i < input.writeData.size(); ++i) {
        input.writeData[i] = i + 1;
    }
    input.dataMask = 0x00ff;
    file.computeNext(input);
    file.commit();

    input = {};
    input.kernel = 3;
    input.readEnable = true;
    file.computeNext(input);
    file.commit();
    EXPECT_FALSE(file.evaluate().valid);

    input.readEnable = false;
    file.computeNext(input);
    file.commit();
    ASSERT_TRUE(file.evaluate().valid);
    EXPECT_EQ(file.evaluate().data[0], 1);
    EXPECT_EQ(file.evaluate().data[7], 8);
    EXPECT_FALSE(file.evaluate().last);
}

TEST(SauShiftRegisterTest, FormsWindowOverRealKernelCycles)
{
    SauShiftRegister shift;
    shift.reset();
    SauShiftRegisterInputs input;
    input.valid = true;
    input.kernel = 3;
    for (unsigned i = 0; i < input.data.size(); ++i) {
        input.data[i] = i;
    }
    shift.computeNext(input);
    shift.commit();
    EXPECT_TRUE(shift.evaluate().valid);
    EXPECT_FALSE(shift.evaluate().last);

    input.valid = false;
    shift.computeNext(input);
    shift.commit();
    EXPECT_TRUE(shift.evaluate().almostLast);
    shift.computeNext(input);
    shift.commit();
    EXPECT_TRUE(shift.evaluate().last);
}

TEST(SauFeederTest, AppliesNineCycleSchedulerLabelToMemoryBeat)
{
    SauFeeder feeder;
    feeder.reset();
    SauFeederInputs input;
    input.start = true;
    input.command.flowLoopTimes = 1;
    feeder.computeNext(input);
    feeder.commit();

    input = {};
    input.schedulerState = SchedulerState::FirstLoad;
    input.inputSwitch = 0;
    for (unsigned i = 0; i <= SauConstants::FeederStateDelay; ++i) {
        feeder.computeNext(input);
        feeder.commit();
    }

    input.memoryValid = true;
    input.memoryData[0] = 0x81;
    feeder.computeNext(input);
    feeder.commit();
    ASSERT_TRUE(feeder.evaluate().aValid);
    EXPECT_EQ(feeder.evaluate().a[0], -127);
    EXPECT_FALSE(feeder.evaluate().bValid);
}

TEST(SauFeederTest, StreamsSixteenOrThirtyTwoWritebackBeats)
{
    for (const bool shift : {false, true}) {
        SauFeeder feeder;
        feeder.reset();
        SauFeederInputs input;
        input.start = true;
        input.command.shift = shift;
        input.command.lastInstruction = true;
        input.command.flowLoopTimes = 1;
        feeder.computeNext(input);
        feeder.commit();

        input.start = false;
        input.schedulerState = SchedulerState::DOut;
        unsigned addressBeats = 0;
        unsigned dataBeats = 0;
        for (unsigned cycle = 0; cycle < 80; ++cycle) {
            input.resultValid = cycle < SauConstants::Rows;
            input.resultLast = cycle == SauConstants::Rows - 1;
            for (auto &value : input.resultData) {
                value = static_cast<int16_t>(cycle + 1);
            }
            feeder.computeNext(input);
            feeder.commit();
            const auto output = feeder.evaluate();
            addressBeats += output.writeAddressValid;
            dataBeats += output.writeDataValid;
        }
        EXPECT_EQ(addressBeats, shift ? 32u : 16u);
        EXPECT_EQ(dataBeats, shift ? 32u : 16u);
    }
}

} // namespace
} // namespace gem5::sau_mikui
