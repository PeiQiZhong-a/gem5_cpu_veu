#include "sau_mikui/sau_constants.hh"
#include "sau_mikui/sau_csr.hh"
#include "sau_mikui/sau_scheduler.hh"

#include <gtest/gtest.h>

namespace gem5::sau_mikui
{
namespace
{

void
tick(SauCsr &csr, const SauCsrInputs &inputs = {})
{
    csr.computeNext(inputs);
    csr.commit();
}

void
tick(SauScheduler &scheduler, const SauSchedulerInputs &inputs)
{
    scheduler.computeNext(inputs);
    scheduler.commit();
}

TEST(SauFixedArithmeticTest, MatchesRtlSaturationAndTruncation)
{
    EXPECT_EQ(saturatingAdd24(0x7fffff, 1), 0x7fffff);
    EXPECT_EQ(saturatingAdd24(-0x800000, -1), -0x800000);
    EXPECT_EQ(saturatingAdd24(-5, 3), -2);
    EXPECT_EQ(saturatingTruncate(0x7fffff, 0, false), 127);
    EXPECT_EQ(saturatingTruncate(-0x800000, 0, false), -128);
    EXPECT_EQ(saturatingTruncate(0x7fffff, 0, true), 32767);
    EXPECT_EQ(saturatingTruncate(-3, 1, true), -2);
}

TEST(SauCsrTest, DecodesOverlappingCommandOneFieldsExactly)
{
    SauCsr csr;
    csr.reset();
    SauCsrInputs inputs;
    inputs.request.write = true;
    inputs.request.writeType = 1;
    inputs.request.address = 0x200;
    inputs.request.writeData = uint64_t{2} | (uint64_t{3} << 2) |
                               (uint64_t{1} << 4) | (uint64_t{1} << 5) |
                               (uint64_t{2} << 32) | (uint64_t{17} << 34) |
                               (uint64_t{1} << 39);
    tick(csr, inputs);

    const auto output = csr.evaluate();
    EXPECT_TRUE(output.ready);
    EXPECT_EQ(output.command.registerMode, 2);
    EXPECT_EQ(output.command.convKernel, 3);
    EXPECT_EQ(output.command.reuseMode, 1);
    EXPECT_TRUE(output.command.stride);
    EXPECT_TRUE(output.command.shift);
    EXPECT_EQ(output.command.transposeMode, TransposeMode::Abtd);
    EXPECT_EQ(output.command.cutbit, 17);
    EXPECT_TRUE(output.command.lastInstruction);
}

TEST(SauCsrTest, StartIsOneCycleAndReadbackIsRegistered)
{
    SauCsr csr;
    csr.reset();
    SauCsrInputs inputs;
    inputs.request.write = true;
    inputs.request.writeType = 1;
    inputs.request.address = 0x206;
    inputs.request.writeData =
        uint64_t{1} | (uint64_t{0x5a} << 1) | (uint64_t{0x12345} << 9) |
        (uint64_t{0x54321} << 32) | (uint64_t{3} << 52) | (uint64_t{2} << 54) |
        (uint64_t{9} << 56);
    tick(csr, inputs);
    ASSERT_TRUE(csr.evaluate().start);
    EXPECT_TRUE(csr.evaluate().crossbarStart);

    tick(csr);
    EXPECT_FALSE(csr.evaluate().start);
    EXPECT_TRUE(csr.evaluate().busy);

    inputs = {};
    inputs.request.read = true;
    inputs.request.address = 0x207;
    tick(csr, inputs);
    ASSERT_TRUE(csr.evaluate().ready);
    EXPECT_EQ(csr.evaluate().readData & 0xfffff, 0x54321u);
    EXPECT_EQ((csr.evaluate().readData >> 20) & 3, 3u);
    EXPECT_EQ((csr.evaluate().readData >> 22) & 3, 2u);
    EXPECT_EQ((csr.evaluate().readData >> 24) & 0x3f, 9u);

    inputs = {};
    inputs.flowEnd = true;
    tick(csr, inputs);
    EXPECT_FALSE(csr.evaluate().busy);
}

TEST(SauCsrTest, FlagsWritesWhileProcessing)
{
    SauCsr csr;
    csr.reset();
    SauCsrInputs inputs;
    inputs.request.write = true;
    inputs.request.writeType = 1;
    inputs.request.address = 0x206;
    inputs.request.writeData = 1;
    tick(csr, inputs);
    tick(csr);
    inputs.request.address = 0x200;
    inputs.request.writeData = 0;
    tick(csr, inputs);
    EXPECT_TRUE(csr.evaluate().crossbarError);
    tick(csr);
    EXPECT_FALSE(csr.evaluate().crossbarError);
}

TEST(SauSchedulerTest, TakesRegisterTransposeReuseAndDoneBranches)
{
    SauScheduler scheduler;
    scheduler.reset();
    SauSchedulerInputs inputs;
    inputs.start = true;
    inputs.convKernel = 3;
    inputs.transposeMode = TransposeMode::Atbd;
    inputs.reuseMode = 1;
    inputs.flowTimes = 1;
    tick(scheduler, inputs);
    inputs.start = false;
    tick(scheduler, inputs);
    EXPECT_EQ(scheduler.evaluate(inputs).state, SchedulerState::RegisterLoad);

    inputs.loadDone = true;
    tick(scheduler, inputs);
    EXPECT_EQ(scheduler.evaluate(inputs).state, SchedulerState::TransposeLoad);
    inputs.loadDone = false;
    for (unsigned cycle = 0; cycle < SauConstants::Rows; ++cycle) {
        tick(scheduler, inputs);
    }
    EXPECT_EQ(scheduler.evaluate(inputs).state, SchedulerState::ReuseLoad);

    inputs.executeFinished = true;
    tick(scheduler, inputs);
    EXPECT_EQ(scheduler.evaluate(inputs).state, SchedulerState::DOut);
    inputs.executeFinished = false;
    inputs.lastInstruction = true;
    inputs.registerFileClear = true;
    EXPECT_TRUE(scheduler.evaluate(inputs).flowEnd);
    tick(scheduler, inputs);
    EXPECT_EQ(scheduler.evaluate(inputs).state, SchedulerState::Idle);
    // crossbar_done is the registered copy of the prior edge's flow_end.
    EXPECT_TRUE(scheduler.evaluate(inputs).crossbarDone);
}

} // anonymous namespace
} // namespace gem5::sau_mikui
