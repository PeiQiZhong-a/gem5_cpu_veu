#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "sau_n/im2col_types.hh"

namespace gem5::sau_n
{
namespace
{

ResolvedConfig
validConfig()
{
    ResolvedConfig config;
    config.name = "w5_pack3_pad1_stride1";
    config.n = 1;
    config.c = 2;
    config.h = 4;
    config.w = 5;
    config.outH = 4;
    config.outW = 5;
    config.kernelH = 3;
    config.kernelW = 3;
    config.strideH = 1;
    config.strideW = 1;
    config.dilationH = 1;
    config.dilationW = 1;
    config.padTop = 1;
    config.padLeft = 1;
    config.spadBase = 0;
    return config;
}

TEST(Im2ColResolvedConfig, DerivesPackedW5Counts)
{
    const auto derived = validateAndDerive(validConfig());

    EXPECT_EQ(derived.rowsPerWord, uint64_t{3});
    EXPECT_EQ(derived.wWords, uint64_t{1});
    EXPECT_EQ(derived.spatialWordsPerChannel, uint64_t{2});
    EXPECT_EQ(derived.totalSpatialWords, uint64_t{4});
    EXPECT_EQ(derived.hGroups, uint64_t{2});
    EXPECT_EQ(derived.wGroups, uint64_t{1});
    EXPECT_EQ(derived.expectedVectors, uint64_t{36});
}

TEST(Im2ColResolvedConfig, DerivesSplitW20Counts)
{
    auto config = validConfig();
    config.name = "w20_split_pad1_stride1";
    config.h = 3;
    config.w = 20;
    config.outH = 3;
    config.outW = 20;

    const auto derived = validateAndDerive(config);
    EXPECT_EQ(derived.rowsPerWord, uint64_t{1});
    EXPECT_EQ(derived.wWords, uint64_t{2});
    EXPECT_EQ(derived.spatialWordsPerChannel, uint64_t{6});
    EXPECT_EQ(derived.totalSpatialWords, uint64_t{12});
    EXPECT_EQ(derived.hGroups, uint64_t{3});
    EXPECT_EQ(derived.wGroups, uint64_t{2});
    EXPECT_EQ(derived.expectedVectors, uint64_t{108});
}

TEST(Im2ColResolvedConfig, RejectsEveryFieldRange)
{
    using Mutator = std::function<void(ResolvedConfig &)>;
    const std::vector<std::pair<std::string, Mutator>> invalid = {
        {"schema_version", [](auto &c) { c.schemaVersion = 2; }},
        {"n", [](auto &c) { c.n = 0; }},
        {"c", [](auto &c) { c.c = 65536; }},
        {"h", [](auto &c) { c.h = 0; }},
        {"w", [](auto &c) { c.w = 65536; }},
        {"out_h", [](auto &c) { c.outH = 0; }},
        {"out_w", [](auto &c) { c.outW = 65536; }},
        {"pad_top", [](auto &c) { c.padTop = 65536; }},
        {"pad_left", [](auto &c) { c.padLeft = 65536; }},
        {"kernel_h", [](auto &c) { c.kernelH = 0; }},
        {"kernel_w", [](auto &c) { c.kernelW = 16; }},
        {"stride_h", [](auto &c) { c.strideH = 0; }},
        {"stride_w", [](auto &c) { c.strideW = 16; }},
        {"dilation_h", [](auto &c) { c.dilationH = 0; }},
        {"dilation_w", [](auto &c) { c.dilationW = 16; }},
        {"spad_base", [](auto &c) { c.spadBase = 4096; }},
        {"cfg_dw_mode", [](auto &c) { c.cfgDwMode = 1; }},
        {"cfg_kernel_pattern",
         [](auto &c) { c.cfgKernelPattern = 0xfffe; }},
    };

    for (const auto &[name, mutate] : invalid) {
        SCOPED_TRACE(name);
        auto config = validConfig();
        mutate(config);
        EXPECT_THROW(validateAndDerive(config), std::invalid_argument);
    }
}

TEST(Im2ColResolvedConfig, RejectsUnsupportedGenerator)
{
    auto config = validConfig();
    config.inputGenerator = "other";
    EXPECT_THROW(validateAndDerive(config), std::invalid_argument);
}

TEST(Im2ColResolvedConfig, RejectsKernelAreaAboveBlockSize)
{
    auto config = validConfig();
    config.kernelH = 5;
    config.kernelW = 4;
    EXPECT_THROW(validateAndDerive(config), std::invalid_argument);
}

TEST(Im2ColResolvedConfig, RejectsUnsupportedPackedOutputWidth)
{
    auto config = validConfig();
    config.outW = 6;
    EXPECT_THROW(validateAndDerive(config), std::invalid_argument);
}

TEST(Im2ColResolvedConfig, ChecksScratchpadFootprintBoundary)
{
    auto config = validConfig();
    config.spadBase = 4092;
    EXPECT_NO_THROW(validateAndDerive(config));

    config.spadBase = 4093;
    EXPECT_THROW(validateAndDerive(config), std::invalid_argument);
}

TEST(Im2ColResolvedConfig, CheckedUint64ArithmeticRejectsOverflow)
{
    constexpr auto Max = std::numeric_limits<uint64_t>::max();
    EXPECT_EQ(checkedAdd(Max - 1, 1, "sum"), Max);
    EXPECT_EQ(checkedMultiply((Max - 1) / 2, 2, "product"), Max - 1);
    EXPECT_THROW(checkedAdd(Max, 1, "sum"), std::invalid_argument);
    EXPECT_THROW(checkedMultiply(Max, 2, "product"), std::invalid_argument);
}

TEST(Im2ColCycleContract, FreezesStateEncodingAndTraceHeader)
{
    EXPECT_EQ(static_cast<uint8_t>(Im2ColState::Idle), uint8_t{0});
    EXPECT_EQ(static_cast<uint8_t>(Im2ColState::Issue), uint8_t{1});
    EXPECT_EQ(static_cast<uint8_t>(Im2ColState::Collect), uint8_t{2});
    EXPECT_EQ(static_cast<uint8_t>(Im2ColState::Push), uint8_t{3});
    EXPECT_EQ(static_cast<uint8_t>(Im2ColState::Next), uint8_t{4});
    EXPECT_EQ(static_cast<uint8_t>(Im2ColState::Done), uint8_t{5});
    EXPECT_EQ(stateName(Im2ColState::Done), "DONE");

    ASSERT_EQ(TraceFields.size(), std::size_t{47});
    EXPECT_EQ(TraceFields[10], "req_addr_b00");
    EXPECT_EQ(TraceFields[25], "req_addr_b15");
    EXPECT_EQ(TraceFields[27], "resp_data_b00");
    EXPECT_EQ(TraceFields[42], "resp_data_b15");
    EXPECT_EQ(TraceFields[43], "feed_valid");
    EXPECT_EQ(TraceFields[46], "feed_mask");
}

TEST(Im2ColCycleContract, CycleZeroIsIssueBusyWithoutDone)
{
    const auto observation = observe(0, cycleZeroRegisters());

    EXPECT_EQ(observation.cycle, uint64_t{0});
    EXPECT_EQ(observation.state, Im2ColState::Issue);
    EXPECT_TRUE(observation.busy);
    EXPECT_FALSE(observation.done);
}

TEST(Im2ColCycleContract, DoneUsesOldNextCommitSemantics)
{
    ControlRegisters old{Im2ColState::Done, false};
    const auto oldObservation = observe(9, old);
    const auto next = doneTransitionNext(old);

    EXPECT_EQ(oldObservation.state, Im2ColState::Done);
    EXPECT_TRUE(oldObservation.busy);
    EXPECT_FALSE(oldObservation.done);
    EXPECT_EQ(old, (ControlRegisters{Im2ColState::Done, false}));

    commit(old, next);
    const auto nextObservation = observe(10, old);
    EXPECT_EQ(nextObservation.state, Im2ColState::Idle);
    EXPECT_FALSE(nextObservation.busy);
    EXPECT_TRUE(nextObservation.done);

    EXPECT_FALSE(beginNext(old).done);
}

TEST(Im2ColCycleContract, DoneTransitionRejectsOtherOldStates)
{
    const ControlRegisters old{Im2ColState::Next, false};
    EXPECT_THROW(doneTransitionNext(old), std::invalid_argument);
}

} // anonymous namespace
} // namespace gem5::sau_n
