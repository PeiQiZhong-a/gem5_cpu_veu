#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "sau_n/sau_types.hh"

namespace gem5::sau_n
{
namespace
{

PipelineResolvedConfig
validConfig()
{
    PipelineResolvedConfig config;
    config.name = "conv_n1_c2_h4_w5_oc3";
    config.im2col.name = "conv_n1_c2_h4_w5_oc3_im2col";
    config.im2col.n = 1;
    config.im2col.c = 2;
    config.im2col.h = 4;
    config.im2col.w = 5;
    config.im2col.outH = 4;
    config.im2col.outW = 5;
    config.im2col.kernelH = 3;
    config.im2col.kernelW = 3;
    config.im2col.strideH = 1;
    config.im2col.strideW = 1;
    config.im2col.dilationH = 1;
    config.im2col.dilationW = 1;
    config.im2col.padTop = 1;
    config.im2col.padLeft = 1;
    config.im2col.spadBase = 0;
    config.outChannels = 3;
    config.cutbit = 8;
    return config;
}

TEST(SauPipelineConfig, DerivesSharedPythonCppAnchor)
{
    const auto derived = validateAndDerive(validConfig());

    EXPECT_EQ(derived.k, uint64_t{18});
    EXPECT_EQ(derived.expectedTiles, uint64_t{2});
    EXPECT_EQ(derived.expectedOutputs, uint64_t{60});
    EXPECT_EQ(derived.expectedMacs, uint64_t{1080});
    EXPECT_EQ(derived.im2col.expectedVectors, uint64_t{36});
}

TEST(SauPipelineConfig, RejectsRangesModesAndGenerators)
{
    using Mutator = std::function<void(PipelineResolvedConfig &)>;
    const std::vector<std::pair<std::string, Mutator>> invalid = {
        {"schema_version", [](auto &c) { c.schemaVersion = 2; }},
        {"empty_name", [](auto &c) { c.name.clear(); }},
        {"channels", [](auto &c) { c.im2col.c = 64; }},
        {"kernel", [](auto &c) { c.im2col.kernelH = 2; }},
        {"out_channels_0", [](auto &c) { c.outChannels = 0; }},
        {"out_channels_17", [](auto &c) { c.outChannels = 17; }},
        {"cutbit", [](auto &c) { c.cutbit = 24; }},
        {"weight_generator", [](auto &c) {
             c.weightGenerator = "random";
         }},
        {"bias_generator", [](auto &c) { c.biasGenerator = "ones"; }},
    };
    for (const auto &[name, mutate] : invalid) {
        SCOPED_TRACE(name);
        auto config = validConfig();
        mutate(config);
        EXPECT_THROW(validateAndDerive(config), std::invalid_argument);
    }
}

TEST(SauPipelineContract, FreezesStatesReadyAndPackedOrdering)
{
    EXPECT_EQ(static_cast<uint8_t>(PipelineState::Idle), uint8_t{0});
    EXPECT_EQ(static_cast<uint8_t>(PipelineState::CollectTile), uint8_t{1});
    EXPECT_EQ(static_cast<uint8_t>(PipelineState::LaunchSa), uint8_t{2});
    EXPECT_EQ(static_cast<uint8_t>(PipelineState::StreamK), uint8_t{3});
    EXPECT_EQ(static_cast<uint8_t>(PipelineState::WaitResult), uint8_t{4});
    EXPECT_EQ(static_cast<uint8_t>(PipelineState::DrainOutput), uint8_t{5});
    EXPECT_EQ(static_cast<uint8_t>(PipelineState::Done), uint8_t{6});
    EXPECT_EQ(pipelineStateName(PipelineState::LaunchSa), "LAUNCH_SA");
    EXPECT_FALSE(SauCycleAnchorsProvisional);

    const OutputReadyConfig ready{5, 2};
    EXPECT_TRUE(outputReady(0, ready));
    EXPECT_TRUE(outputReady(1, ready));
    EXPECT_FALSE(outputReady(2, ready));
    EXPECT_TRUE(outputReady(5, ready));
    EXPECT_THROW(outputReady(0, {0, 1}), std::invalid_argument);
    EXPECT_THROW(outputReady(0, {2, 3}), std::invalid_argument);

    EXPECT_EQ(peIndex(0, 0), uint64_t{0});
    EXPECT_EQ(peIndex(0, 1), uint64_t{1});
    EXPECT_EQ(peIndex(1, 15), uint64_t{31});
    EXPECT_EQ(peIndex(15, 15), uint64_t{255});
    EXPECT_THROW(peIndex(16, 0), std::invalid_argument);
}

TEST(SauPipelineContract, DrainedRequiresEveryFrozenCondition)
{
    const auto derived = validateAndDerive(validConfig());
    PipelineDrainStatus status;
    status.im2colFifoEmpty = true;
    status.tileBufferEmpty = true;
    status.sauIdle = true;
    status.completedTiles = derived.expectedTiles;
    status.writtenOutputs = derived.expectedOutputs;
    EXPECT_TRUE(pipelineDrained(status, derived));

    status.outputPending = true;
    EXPECT_FALSE(pipelineDrained(status, derived));
    status.outputPending = false;
    --status.completedTiles;
    EXPECT_FALSE(pipelineDrained(status, derived));
}

} // anonymous namespace
} // namespace gem5::sau_n
