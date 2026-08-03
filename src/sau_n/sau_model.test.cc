#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>

#include "sau_n/sau_model.hh"

namespace gem5::sau_n
{
namespace
{

TEST(SauNumericHelpers, MultipliesSignedInt8Exactly)
{
    EXPECT_EQ(multiplySignedInt8(-128, -128), int16_t{16384});
    EXPECT_EQ(multiplySignedInt8(-128, 127), int16_t{-16256});
    EXPECT_EQ(multiplySignedInt8(-1, -1), int16_t{1});
    EXPECT_EQ(multiplySignedInt8(127, 127), int16_t{16129});
}

TEST(SauNumericHelpers, SaturatesEverySigned24Add)
{
    EXPECT_EQ(
        saturatingAddSigned24(Accumulator24Max - 1, 1),
        Accumulator24Max);
    EXPECT_EQ(
        saturatingAddSigned24(Accumulator24Max, 1),
        Accumulator24Max);
    EXPECT_EQ(
        saturatingAddSigned24(Accumulator24Min + 1, -1),
        Accumulator24Min);
    EXPECT_EQ(
        saturatingAddSigned24(Accumulator24Min, -1),
        Accumulator24Min);

    const auto saturated = saturatingAddSigned24(Accumulator24Max - 5, 10);
    EXPECT_EQ(saturated, Accumulator24Max);
    EXPECT_EQ(
        saturatingAddSigned24(saturated, -10),
        Accumulator24Max - 10);
    EXPECT_THROW(
        saturatingAddSigned24(Accumulator24Max + 1, 0),
        std::invalid_argument);
    EXPECT_THROW(
        saturatingAddSigned24(0, Accumulator24Max + 1),
        std::invalid_argument);
}

TEST(SauNumericHelpers, QuantizesWithArithmeticShiftAndSignedSlot)
{
    EXPECT_EQ(quantizeSignedInt8(-1, 1), int8_t{-1});
    EXPECT_EQ(quantizeSignedInt8(-255, 1), int8_t{-128});
    EXPECT_EQ(quantizeSignedInt8(-257, 1), int8_t{-128});
    EXPECT_EQ(quantizeSignedInt8(254, 1), int8_t{127});
    EXPECT_EQ(quantizeSignedInt8(256, 1), int8_t{127});
    EXPECT_EQ(signExtendedInt8Slot(-128), uint16_t{0xff80});
    EXPECT_EQ(signExtendedInt8Slot(-1), uint16_t{0xffff});
    EXPECT_EQ(signExtendedInt8Slot(127), uint16_t{0x007f});
    EXPECT_THROW(quantizeSignedInt8(0, 24), std::invalid_argument);
}

TEST(SauNumericCore, ComputesIndependentTwoByThreeVector)
{
    SauNumericCore core;
    core.begin(2, 3);

    SauNumericCore::Int8Lanes activations{};
    SauNumericCore::Int8Lanes weights{};
    activations[0] = 2;
    activations[1] = -3;
    weights[0] = -4;
    weights[1] = 5;
    weights[2] = 6;
    core.macStep(activations, weights);

    activations[0] = 1;
    activations[1] = 1;
    core.macStep(activations, weights);

    SauNumericCore::BiasLanes biases{};
    biases[0] = 1;
    biases[1] = -2;
    biases[2] = 3;
    core.addBias(biases);
    const auto outputs = core.outputSnapshot(0);

    EXPECT_EQ(core.macSteps(), uint64_t{2});
    EXPECT_EQ(core.phase(), SauNumericPhase::Finalized);
    EXPECT_EQ(core.pe(0, 0).product, int16_t{-4});
    EXPECT_EQ(core.pe(1, 2).product, int16_t{6});
    EXPECT_EQ(outputs[peIndex(0, 0)].accumulator, int32_t{-11});
    EXPECT_EQ(outputs[peIndex(0, 1)].accumulator, int32_t{13});
    EXPECT_EQ(outputs[peIndex(0, 2)].accumulator, int32_t{21});
    EXPECT_EQ(outputs[peIndex(1, 0)].accumulator, int32_t{9});
    EXPECT_EQ(outputs[peIndex(1, 1)].accumulator, int32_t{-12});
    EXPECT_EQ(outputs[peIndex(1, 2)].accumulator, int32_t{-9});
    EXPECT_EQ(outputs[peIndex(1, 1)].rtlSlot, uint16_t{0xfff4});
}

TEST(SauNumericCore, NormalizesRowAndColumnTailToZero)
{
    SauNumericCore core;
    core.begin(1, 2);
    SauNumericCore::Int8Lanes activations{};
    SauNumericCore::Int8Lanes weights{};
    activations.fill(7);
    weights.fill(9);
    core.macStep(activations, weights);
    core.addBias({});
    const auto outputs = core.outputSnapshot(0);

    EXPECT_TRUE(outputs[peIndex(0, 0)].valid);
    EXPECT_TRUE(outputs[peIndex(0, 1)].valid);
    EXPECT_FALSE(outputs[peIndex(0, 2)].valid);
    EXPECT_FALSE(outputs[peIndex(1, 0)].valid);
    EXPECT_EQ(outputs[peIndex(0, 2)].accumulator, int32_t{0});
    EXPECT_EQ(outputs[peIndex(1, 0)].rtlSlot, uint16_t{0});
    EXPECT_EQ(core.pe(0, 2), SauPeNumericState{});
    EXPECT_EQ(core.pe(1, 0), SauPeNumericState{});
}

TEST(SauNumericCore, ResetAndClearRemoveAllOperationState)
{
    SauNumericCore core;
    EXPECT_THROW(core.begin(0, 1), std::invalid_argument);
    EXPECT_THROW(core.begin(1, 17), std::invalid_argument);
    EXPECT_THROW(core.macStep({}, {}), std::logic_error);
    EXPECT_THROW(core.addBias({}), std::logic_error);
    EXPECT_THROW(core.outputSnapshot(0), std::logic_error);

    core.begin(16, 16);
    core.macStep({}, {});
    core.reset();
    EXPECT_EQ(core.phase(), SauNumericPhase::Empty);
    EXPECT_EQ(core.validRows(), uint64_t{0});
    EXPECT_EQ(core.validColumns(), uint64_t{0});
    EXPECT_EQ(core.macSteps(), uint64_t{0});
    for (const auto &state : core.peStates()) {
        EXPECT_EQ(state, SauPeNumericState{});
    }

    core.begin(1, 1);
    EXPECT_THROW(core.addBias({}), std::logic_error);
    core.clear();
    EXPECT_EQ(core.phase(), SauNumericPhase::Empty);
}

TEST(SauNumericCore, PositiveK567FirstSaturatesAtMac512)
{
    SauNumericCore core;
    core.begin(1, 1);
    SauNumericCore::Int8Lanes activations{};
    SauNumericCore::Int8Lanes weights{};
    activations[0] = -128;
    weights[0] = -128;
    for (uint64_t mac = 1; mac <= 567; ++mac) {
        core.macStep(activations, weights);
        if (mac == 511) {
            EXPECT_EQ(core.pe(0, 0).accumulator, int32_t{8372224});
        }
        if (mac >= 512) {
            EXPECT_EQ(core.pe(0, 0).accumulator, Accumulator24Max);
        }
    }
    SauNumericCore::BiasLanes biases{};
    biases[0] = 1;
    core.addBias(biases);
    const auto output = core.outputSnapshot(0)[peIndex(0, 0)];
    EXPECT_EQ(output.accumulator, Accumulator24Max);
    EXPECT_EQ(output.value, int8_t{127});
}

TEST(SauNumericCore, NegativeK567FirstSaturatesAtMac517)
{
    SauNumericCore core;
    core.begin(1, 1);
    SauNumericCore::Int8Lanes activations{};
    SauNumericCore::Int8Lanes weights{};
    activations[0] = -128;
    weights[0] = 127;
    for (uint64_t mac = 1; mac <= 567; ++mac) {
        core.macStep(activations, weights);
        if (mac == 516) {
            EXPECT_EQ(core.pe(0, 0).accumulator, int32_t{-8388096});
        }
        if (mac >= 517) {
            EXPECT_EQ(core.pe(0, 0).accumulator, Accumulator24Min);
        }
    }
    SauNumericCore::BiasLanes biases{};
    biases[0] = -1;
    core.addBias(biases);
    const auto output = core.outputSnapshot(0)[peIndex(0, 0)];
    EXPECT_EQ(output.accumulator, Accumulator24Min);
    EXPECT_EQ(output.value, int8_t{-128});
    EXPECT_EQ(output.rtlSlot, uint16_t{0xff80});
}

} // anonymous namespace
} // namespace gem5::sau_n
