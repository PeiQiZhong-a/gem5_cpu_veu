#include "sau_mikui/sau_array_engine.hh"
#include "sau_mikui/sau_output_path.hh"
#include "sau_mikui/sau_pe.hh"

#include <gtest/gtest.h>

namespace gem5::sau_mikui
{
namespace
{

void
tick(SauPe &pe, const SauPeInputs &inputs = {})
{
    pe.computeNext(inputs);
    pe.commit();
}

TEST(SauPeTest, RetainsIndependentRegisteredMultiplyAndAccumulatorState)
{
    SauPe pe;
    pe.reset();
    SauPeInputs inputs;
    inputs.writeStrobe = true;
    tick(pe, inputs); // establish the row write-strobe pipeline

    inputs.enable = true;
    inputs.activation = 2;
    inputs.weight = -3;
    tick(pe, inputs);
    inputs.enable = false;
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        tick(pe, inputs);
    }
    EXPECT_EQ(pe.evaluate().accumulator, -6);

    inputs.accumulatorFinish = true;
    tick(pe, inputs);
    ASSERT_TRUE(pe.evaluate().valid);
    EXPECT_EQ(pe.evaluate().accumulator, -6);
    inputs.accumulatorFinish = false;
    tick(pe, inputs);
    EXPECT_EQ(pe.evaluate().result, -6);
    tick(pe, inputs);
    EXPECT_EQ(pe.evaluate().accumulator, 0);
}

TEST(SauPeTest, KeepModePreservesAccumulator)
{
    SauPe pe;
    pe.reset();
    SauPeInputs inputs;
    inputs.writeStrobe = true;
    inputs.instruction.keepMode = true;
    tick(pe, inputs);
    inputs.enable = true;
    inputs.activation = 7;
    inputs.weight = 5;
    tick(pe, inputs);
    inputs.enable = false;
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        tick(pe, inputs);
    }
    inputs.accumulatorFinish = true;
    tick(pe, inputs);
    inputs.accumulatorFinish = false;
    tick(pe, inputs);
    tick(pe, inputs);
    EXPECT_EQ(pe.evaluate().accumulator, 35);
}

TEST(SauPeArrayTest, OwnsAllTwoHundredFiftySixPeStates)
{
    SauPeArray array;
    SauPeArrayInputs inputs;
    inputs.writeStrobe.fill(true);
    inputs.enable = true;
    inputs.activations.fill(1);
    inputs.weights.fill(1);
    for (unsigned cycle = 0; cycle < 40; ++cycle) {
        array.computeNext(inputs);
        array.commit();
    }
    const auto outputs = array.evaluate();
    EXPECT_GT(outputs.accumulators[0][0], 0);
    EXPECT_GT(outputs.accumulators[15][15], 0);
    EXPECT_NE(outputs.accumulators[0][0], outputs.accumulators[15][15]);
}

TEST(SauOutputPathTest, SaturatesEightAndSixteenBitResults)
{
    SauOutputPath path;
    path.reset();
    SauOutputPathInputs input;
    input.valid = true;
    input.row[0] = 1000;
    input.row[1] = -1000;
    path.computeNext(input);
    path.commit();
    EXPECT_EQ(path.evaluate().row[0], 127);
    EXPECT_EQ(path.evaluate().row[1], -128);

    input.shift = true;
    input.row[0] = 100000;
    input.row[1] = -100000;
    path.computeNext(input);
    path.commit();
    EXPECT_EQ(path.evaluate().row[0], 32767);
    EXPECT_EQ(path.evaluate().row[1], -32768);
}

TEST(SauArrayEngineTest, RotatesDepthwiseColumnMaskPerKernelWindow)
{
    SauArrayEngine engine;
    engine.reset();
    SauArrayEngineInputs inputs;
    inputs.start = true;
    inputs.command.operation = CalculateMode::Conv;
    inputs.command.convKernel = 3;
    inputs.command.registerMode = 2;
    engine.computeNext(inputs);
    engine.commit();

    inputs = {};
    inputs.bValid = true;
    inputs.b.fill(1);
    engine.computeNext(inputs);
    engine.commit();
    for (unsigned cycle = 0; cycle < 9; ++cycle) {
        inputs = {};
        inputs.aValid = true;
        inputs.a.fill(1);
        inputs.aLast = cycle == 8;
        engine.computeNext(inputs);
        engine.commit();
    }
    for (unsigned cycle = 0;
         cycle < 80 && engine.evaluate().state != ArrayEngineState::Storage;
         ++cycle) {
        engine.computeNext({});
        engine.commit();
    }
    ASSERT_EQ(engine.evaluate().state, ArrayEngineState::Storage);
    inputs = {};
    inputs.outputReady = true;
    engine.computeNext(inputs);
    engine.commit();
    const auto output = engine.evaluate();
    ASSERT_TRUE(output.rowValid);
    EXPECT_EQ(output.row[0], 9);
    for (unsigned col = 1; col < SauConstants::Cols; ++col) {
        EXPECT_EQ(output.row[col], 0);
    }
}

TEST(SauArrayEngineTest, MatchesRtlRetainThenCnormalMilestones)
{
    SauArrayEngine engine;
    engine.reset();
    unsigned cycle = 0;
    auto clock = [&](const SauArrayEngineInputs &inputs = {}) {
        engine.computeNext(inputs);
        engine.commit();
        ++cycle;
        return engine.evaluate();
    };

    SauArrayEngineInputs inputs;
    inputs.start = true;
    inputs.command.flowMode = FlowMode::Retain;
    clock(inputs);

    const unsigned firstInputCycle = cycle + 1;
    unsigned firstPeFinish = 0;
    for (unsigned beat = 0; beat < SauConstants::Rows; ++beat) {
        inputs = {};
        inputs.aValid = true;
        inputs.bValid = true;
        inputs.a.fill(1);
        inputs.b.fill(1);
        inputs.aLast = beat == SauConstants::Rows - 1;
        inputs.bLast = inputs.aLast;
        const auto output = clock(inputs);
        if (output.peFinish) {
            firstPeFinish = cycle;
        }
    }
    while (engine.evaluate().state != ArrayEngineState::Storage &&
           cycle < 100) {
        const auto output = clock();
        EXPECT_FALSE(output.rowValid);
        if (output.peFinish) {
            firstPeFinish = cycle;
        }
    }
    ASSERT_EQ(engine.evaluate().state, ArrayEngineState::Storage);
    const unsigned firstStorage = cycle;
    EXPECT_EQ(firstPeFinish - firstInputCycle, 21u);
    EXPECT_EQ(firstStorage - firstInputCycle, 37u);

    inputs = {};
    inputs.start = true;
    inputs.command.flowMode = FlowMode::Cnormal;
    clock(inputs);
    const unsigned secondInputCycle = cycle + 1;
    unsigned secondPeFinish = 0;
    for (unsigned beat = 0; beat < SauConstants::Rows; ++beat) {
        inputs = {};
        inputs.aValid = true;
        inputs.bValid = true;
        inputs.a.fill(1);
        inputs.b.fill(1);
        inputs.aLast = beat == SauConstants::Rows - 1;
        inputs.bLast = inputs.aLast;
        const auto output = clock(inputs);
        if (output.peFinish) {
            secondPeFinish = cycle;
        }
    }
    while (engine.evaluate().state != ArrayEngineState::Storage &&
           cycle < 200) {
        const auto output = clock();
        if (output.peFinish) {
            secondPeFinish = cycle;
        }
    }
    ASSERT_EQ(engine.evaluate().state, ArrayEngineState::Storage)
        << "cycle=" << cycle << " second_input=" << secondInputCycle
        << " pe_finish=" << secondPeFinish;
    const unsigned secondStorage = cycle;
    EXPECT_EQ(secondPeFinish - secondInputCycle, 21u);
    EXPECT_EQ(secondStorage - secondInputCycle, 37u);

    for (unsigned row = 0; row < SauConstants::Rows; ++row) {
        const auto output = clock();
        ASSERT_TRUE(output.rowValid) << row;
        EXPECT_EQ(output.rowIndex, row);
        for (const auto value : output.row) {
            EXPECT_EQ(value, 32) << row;
        }
    }
    EXPECT_EQ(cycle - secondInputCycle, 53u);
}

} // anonymous namespace
} // namespace gem5::sau_mikui
