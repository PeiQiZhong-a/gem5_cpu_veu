#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <vector>

#include "sau_n/im2col_address.hh"
#include "sau_n/sau_generators.hh"
#include "sau_n/streaming_conv_pipeline_model.hh"

namespace gem5::sau_n
{
namespace
{

PipelineResolvedConfig
streamingConfig()
{
    PipelineResolvedConfig config;
    config.name = "streaming_pipeline";
    config.im2col.name = "streaming_pipeline_im2col";
    config.im2col.n = 1;
    config.im2col.c = 2;
    config.im2col.h = 4;
    config.im2col.w = 6;
    config.im2col.outH = 2;
    config.im2col.outW = 3;
    config.im2col.kernelH = 3;
    config.im2col.kernelW = 3;
    config.im2col.strideH = 2;
    config.im2col.strideW = 2;
    config.im2col.dilationH = 1;
    config.im2col.dilationW = 1;
    config.im2col.padTop = 1;
    config.im2col.padLeft = 1;
    config.outChannels = 3;
    config.cutbit = 8;
    return config;
}

int32_t
sat24(int32_t accumulator, int32_t addend)
{
    const int64_t sum = static_cast<int64_t>(accumulator) + addend;
    return static_cast<int32_t>(std::clamp<int64_t>(
        sum, -(int64_t{1} << 23), (int64_t{1} << 23) - 1));
}

int8_t
directActivation(uint64_t n, uint64_t c, uint64_t h, uint64_t w)
{
    const uint64_t raw = (n * 97 + c * 31 + h * 7 + w + 1) % 256;
    return static_cast<int8_t>(
        raw < 128 ? static_cast<int16_t>(raw) :
        static_cast<int16_t>(raw) - 256);
}

int8_t
directWeight(uint64_t oc, uint64_t c, uint64_t kh, uint64_t kw)
{
    const uint64_t raw =
        (oc * 29 + c * 17 + kh * 5 + kw * 3 + 11) % 255;
    return static_cast<int8_t>(static_cast<int16_t>(raw) - 127);
}

std::vector<int8_t>
directOracle(const PipelineResolvedConfig &config)
{
    std::vector<int8_t> outputs;
    for (uint64_t n = 0; n < config.im2col.n; ++n) {
        for (uint64_t oc = 0; oc < config.outChannels; ++oc) {
            for (uint64_t oh = 0; oh < config.im2col.outH; ++oh) {
                for (uint64_t ow = 0; ow < config.im2col.outW; ++ow) {
                    int32_t accumulator = 0;
                    for (uint64_t c = 0; c < config.im2col.c; ++c) {
                        for (uint64_t kh = 0; kh < 3; ++kh) {
                            for (uint64_t kw = 0; kw < 3; ++kw) {
                                const int64_t ih = static_cast<int64_t>(
                                    oh * config.im2col.strideH + kh) -
                                    static_cast<int64_t>(
                                        config.im2col.padTop);
                                const int64_t iw = static_cast<int64_t>(
                                    ow * config.im2col.strideW + kw) -
                                    static_cast<int64_t>(
                                        config.im2col.padLeft);
                                int8_t activation = 0;
                                if (ih >= 0 && iw >= 0 &&
                                    ih < static_cast<int64_t>(
                                        config.im2col.h) &&
                                    iw < static_cast<int64_t>(
                                        config.im2col.w)) {
                                    activation = directActivation(
                                        n, c, static_cast<uint64_t>(ih),
                                        static_cast<uint64_t>(iw));
                                }
                                accumulator = sat24(
                                    accumulator,
                                    static_cast<int32_t>(activation) *
                                        directWeight(oc, c, kh, kw));
                            }
                        }
                    }
                    const int32_t bias = static_cast<int32_t>(
                        (oc * 37 + 13) % 257) - 128;
                    accumulator = sat24(accumulator, bias);
                    const int64_t divisor = int64_t{1} << config.cutbit;
                    const int64_t shifted = accumulator >= 0 ?
                        accumulator / divisor :
                        -((-static_cast<int64_t>(accumulator) +
                           divisor - 1) / divisor);
                    outputs.push_back(static_cast<int8_t>(
                        std::clamp<int64_t>(shifted, -128, 127)));
                }
            }
        }
    }
    return outputs;
}

struct RunObservations
{
    uint64_t drainedCycle = 0;
    bool sawNoEmptyBypass = false;
    bool sawLaunchSeparation = false;
    bool sawBusyNextTile = false;
    bool sawFullPopPush = false;
    bool sawDDepthOneTurnover = false;
    bool sawDWritebackOnly = false;
};

RunObservations
runToDrained(StreamingConvPipelineModel &model)
{
    RunObservations result;
    std::optional<uint64_t> beginLaunchCycle;
    std::optional<uint64_t> instructionCycle;
    for (uint64_t attempts = 0; attempts < 200000; ++attempts) {
        const auto cycle = model.tick();
        const auto hasRequest = [](const SramRequest &request) {
            return std::any_of(
                request.valid.begin(), request.valid.end(),
                [](bool valid) { return valid; });
        };
        if (hasRequest(cycle.dRequest)) {
            const bool readGrant =
                hasRequest(cycle.producer.grant) ||
                hasRequest(cycle.bGrant) || hasRequest(cycle.cGrant);
            EXPECT_FALSE(readGrant);
            if (hasRequest(cycle.dGrant)) {
                result.sawDWritebackOnly = true;
            }
        }
        if (cycle.fifoPush && cycle.fifoCount == 0) {
            EXPECT_FALSE(cycle.fifoPop);
            result.sawNoEmptyBypass = true;
        }
        if (cycle.consumer.beginLaunch) {
            beginLaunchCycle = cycle.cycle;
            EXPECT_FALSE(cycle.fifoPop);
        }
        if (cycle.consumer.launch) {
            instructionCycle = cycle.cycle;
            if (!beginLaunchCycle) {
                throw std::logic_error(
                    "instruction launched without IDLE transition");
            }
            EXPECT_EQ(*instructionCycle, *beginLaunchCycle + 1);
            EXPECT_TRUE(cycle.sauInputs.insValid);
            EXPECT_FALSE(cycle.sauInputs.inputValid);
            EXPECT_FALSE(cycle.fifoPop);
        }
        if (cycle.consumer.inputFire && instructionCycle &&
            cycle.acceptedK == 0) {
            EXPECT_EQ(cycle.cycle, *instructionCycle + 1);
            result.sawLaunchSeparation = true;
        }
        if ((cycle.consumerState == StreamingConsumerState::WaitResult ||
             cycle.consumerState == StreamingConsumerState::DrainOutput) &&
            cycle.fifoHeadValid && cycle.fifoHead.tag.tileFirst) {
            EXPECT_FALSE(cycle.fifoPop);
            result.sawBusyNextTile = true;
        }
        if (cycle.fifoCount == StreamingFifoDepth &&
            cycle.fifoPush && cycle.fifoPop) {
            result.sawFullPopPush = true;
        }
        if (cycle.dQueueOccupancy == 1 &&
            cycle.dHeadWillRetire && cycle.dDequeue &&
            cycle.dEnqueue) {
            result.sawDDepthOneTurnover = true;
        }
        if (cycle.drained) {
            result.drainedCycle = cycle.cycle;
            return result;
        }
    }
    throw std::runtime_error("streaming pipeline did not drain");
}

TEST(StreamingConvPipeline, OneTileLaunchAndOutputAreCanonical)
{
    auto config = streamingConfig();
    config.im2col.c = 1;
    config.im2col.h = config.im2col.w = 3;
    config.im2col.outH = config.im2col.outW = 1;
    config.im2col.strideH = config.im2col.strideW = 1;
    config.im2col.padTop = config.im2col.padLeft = 0;
    config.outChannels = 2;
    config.cutbit = 0;
    config.weightGenerator = "ones";
    config.biasGenerator = "zero";

    StreamingConvPipelineModel model(config);
    const auto run = runToDrained(model);
    EXPECT_EQ(
        StreamingConvPipelineModel::InputProtocol,
        SauInputProtocol::ElasticBubbleEnabled);
    EXPECT_EQ(
        model.producerMemoryMode(),
        PipelinedIm2ColMemoryMode::SharedOneCycle);
    EXPECT_EQ(model.outputs(), (std::vector<int8_t>{81, 81}));
    EXPECT_TRUE(run.sawNoEmptyBypass);
    EXPECT_TRUE(run.sawLaunchSeparation);
    EXPECT_EQ(model.stats().fifoPushes, uint64_t{9});
    EXPECT_EQ(model.stats().fifoPops, uint64_t{9});
    EXPECT_EQ(model.stats().peInputCycles, uint64_t{9});
    EXPECT_EQ(model.stats().peLaunches, uint64_t{1});
    EXPECT_EQ(model.stats().tilesGenerated, uint64_t{1});
    EXPECT_EQ(model.stats().tilesLaunched, uint64_t{1});
    EXPECT_EQ(model.stats().tilesCompleted, uint64_t{1});
    EXPECT_EQ(model.stats().outputRows, uint64_t{1});
    EXPECT_EQ(model.stats().outputElements, uint64_t{2});
    EXPECT_EQ(model.stats().spadReadRequestsC, uint64_t{4});
    EXPECT_EQ(model.stats().spadReadGrantsC, uint64_t{4});
    EXPECT_EQ(model.stats().spadReadResponsesC, uint64_t{4});
    EXPECT_EQ(model.stats().spadWriteGrantsD, uint64_t{2});
    EXPECT_EQ(model.stats().dPendingPeak, uint64_t{1});
    EXPECT_GT(model.stats().spadReadRequestsA, uint64_t{0});
    EXPECT_EQ(
        model.stats().spadReadGrantsA,
        model.stats().spadReadResponsesA);
    EXPECT_LE(
        model.stats().spadReadGrantsA,
        model.stats().spadReadRequestsA);
    const uint64_t perBankReads = std::accumulate(
        model.stats().perBankReadCycles.begin(),
        model.stats().perBankReadCycles.end(), uint64_t{0});
    EXPECT_EQ(
        perBankReads,
        model.stats().spadReadGrantsA +
        model.stats().spadReadGrantsB +
        model.stats().spadReadGrantsC);
    EXPECT_EQ(
        std::accumulate(
            model.stats().perBankWriteCycles.begin(),
            model.stats().perBankWriteCycles.end(), uint64_t{0}),
        model.stats().spadWriteGrantsD);
    EXPECT_GT(model.stats().bBufferPeakOccupancy, uint64_t{0});
    EXPECT_EQ(
        model.producerStats().pipelineFillCycles, uint64_t{4});
    EXPECT_EQ(
        model.producerStats().compactedSpatialVectors, uint64_t{9});
    ASSERT_TRUE(model.drainedCycle());
    EXPECT_EQ(*model.drainedCycle(), run.drainedCycle);
    EXPECT_THROW(model.tick(), std::logic_error);
}

TEST(StreamingConvPipeline, PreloadsSharedABCAreasAndClearsD)
{
    const auto config = streamingConfig();
    const StreamingConvPipelineModel model(config);
    const auto &scratchpad = model.sharedSpad();
    const auto &derived = model.derived();

    const ChwAddressMapper mapper(config.im2col);
    const auto activation = mapper.locate(0, 1, 3, 5);
    EXPECT_EQ(
        scratchpad.read(activation.bank, activation.row),
        tbActValueV1(0, 1, 3, 5));

    const auto weight = bAddress(config, derived, 17, 2);
    EXPECT_EQ(
        scratchpad.read(weight.bank, weight.row),
        static_cast<uint8_t>(
            weightValue(config.weightGenerator, 2, 1, 2, 2)));

    const auto biasLow = cAddress(config, derived, 2, 0);
    const auto biasHigh = cAddress(config, derived, 2, 1);
    const uint16_t reconstructed =
        scratchpad.read(biasLow.bank, biasLow.row) |
        (uint16_t{scratchpad.read(biasHigh.bank, biasHigh.row)} << 8);
    EXPECT_EQ(
        reconstructed,
        static_cast<uint16_t>(biasValue(config.biasGenerator, 2)));
    EXPECT_EQ(
        static_cast<int16_t>(reconstructed),
        biasValue(config.biasGenerator, 2));

    const auto output = dAddress(config, derived, 0, 1, 2, 2);
    EXPECT_EQ(scratchpad.read(output.bank, output.row), uint8_t{0});
}

TEST(StreamingConvPipeline, ReadsSignedBiasBytesBeforeFirstLaunch)
{
    auto config = streamingConfig();
    config.im2col.c = 1;
    config.im2col.h = config.im2col.w = 3;
    config.im2col.outH = config.im2col.outW = 1;
    config.im2col.strideH = config.im2col.strideW = 1;
    config.im2col.padTop = config.im2col.padLeft = 0;
    config.outChannels = 3;

    StreamingConvPipelineModel model(config);
    bool sawLaunch = false;
    for (uint64_t attempts = 0; attempts < 1000; ++attempts) {
        const auto cycle = model.tick();
        if (cycle.cRequest.valid[0]) {
            EXPECT_FALSE(cycle.producer.s0Valid);
            EXPECT_FALSE(cycle.bRequest.valid[0]);
        }
        if (!cycle.consumer.launch) {
            continue;
        }
        sawLaunch = true;
        ASSERT_TRUE(cycle.sauInputs.insValid);
        for (uint64_t column = 0;
             column < config.outChannels; ++column) {
            EXPECT_EQ(
                cycle.sauInputs.config.biases[column],
                biasValue(config.biasGenerator, column));
        }
        break;
    }
    ASSERT_TRUE(sawLaunch);
    EXPECT_LT(model.stats().spadReadResponsesC, uint64_t{7});
    EXPECT_EQ(
        model.stats().spadReadResponsesC,
        config.outChannels * 2);
    EXPECT_EQ(
        model.stats().spadReadGrantsC,
        model.stats().spadReadRequestsC);
}

TEST(StreamingConvPipeline, WritesDAndRebuildsOutputFromScratchpad)
{
    auto config = streamingConfig();
    StreamingConvPipelineModel model(config);
    const auto run = runToDrained(model);
    const auto expected = directOracle(config);

    EXPECT_EQ(model.outputs(), expected);
    EXPECT_TRUE(run.sawDDepthOneTurnover);
    EXPECT_TRUE(run.sawDWritebackOnly);
    EXPECT_EQ(
        model.stats().spadWriteGrantsD,
        model.derived().expectedOutputs);
    EXPECT_GE(
        model.stats().spadWriteRequestsD,
        model.stats().spadWriteGrantsD);
    EXPECT_EQ(model.stats().dPendingPeak, uint64_t{1});
    for (uint64_t n = 0; n < config.im2col.n; ++n) {
        for (uint64_t column = 0;
             column < config.outChannels; ++column) {
            for (uint64_t oh = 0; oh < config.im2col.outH; ++oh) {
                for (uint64_t ow = 0; ow < config.im2col.outW; ++ow) {
                    const uint64_t index =
                        ((n * config.outChannels + column) *
                         config.im2col.outH + oh) *
                        config.im2col.outW + ow;
                    const auto address = dAddress(
                        config, model.derived(), n, oh, ow, column);
                    EXPECT_EQ(
                        signedInt8(model.sharedSpad().read(
                            address.bank, address.row)),
                        expected[static_cast<std::size_t>(index)]);
                }
            }
        }
    }
}

TEST(StreamingConvPipeline, W6Stride2CompactionMatchesDirectOracle)
{
    auto config = streamingConfig();
    config.im2col.n = 2;
    StreamingConvPipelineModel model(config);
    const auto run = runToDrained(model);

    EXPECT_EQ(model.outputs(), directOracle(config));
    EXPECT_TRUE(run.sawLaunchSeparation);
    EXPECT_EQ(
        model.producerStats().rawScatteredMaskVectors,
        model.derived().im2col.expectedVectors);
    EXPECT_EQ(
        model.producerStats().compactedSpatialVectors,
        model.derived().im2col.expectedVectors);
    EXPECT_EQ(model.stats().peInputCycles, uint64_t{36});
    EXPECT_GT(model.stats().peInputBubbleCycles, uint64_t{0});
    EXPECT_EQ(model.stats().bBufferFillVectors, model.derived().k);
    EXPECT_EQ(
        model.stats().bBufferConsumedVectors,
        model.derived().im2col.expectedVectors);
    EXPECT_EQ(
        model.stats().weightReuseHits,
        (model.derived().expectedTiles - 1) * model.derived().k);
    EXPECT_EQ(
        model.stats().spadReadResponsesB,
        model.derived().k * config.outChannels);
    EXPECT_GT(model.stats().bPrefetchStallCycles, uint64_t{0});
}

TEST(StreamingConvPipeline, PrefetchesNextTileAndExchangesFullFifo)
{
    auto config = streamingConfig();
    config.im2col.h = 4;
    config.im2col.w = 32;
    config.im2col.outH = 4;
    config.im2col.outW = 32;
    config.im2col.strideH = config.im2col.strideW = 1;
    config.outChannels = 4;
    StreamingConvPipelineModel model(config, {7, 2});
    const auto run = runToDrained(model);

    EXPECT_EQ(model.outputs(), directOracle(config));
    EXPECT_TRUE(run.sawBusyNextTile);
    EXPECT_TRUE(run.sawFullPopPush);
    EXPECT_EQ(
        model.stats().fifoPeakOccupancy, StreamingFifoDepth);
    EXPECT_GT(model.stats().fifoFullCycles, uint64_t{0});
    EXPECT_GT(model.producerStats().s2StallCycles, uint64_t{0});
    EXPECT_EQ(
        model.stats().fifoPushes,
        model.derived().im2col.expectedVectors);
    EXPECT_EQ(model.stats().tilesCompleted, model.derived().expectedTiles);
    EXPECT_EQ(model.stats().outputElements, model.derived().expectedOutputs);
}

TEST(StreamingConvPipeline, SmallBChunksRefillAndConsumeGlobalKInOrder)
{
    auto config = streamingConfig();
    config.im2col.n = 2;
    config.sharedSpad =
        validateStreamingConfig(config).sharedSpad;
    config.sharedSpad.configured = true;
    config.sharedSpad.bBufferDepth = 2;
    config.sharedSpad.weightReuse = false;

    StreamingConvPipelineModel model(config);
    uint64_t inputFires = 0;
    for (uint64_t attempts = 0; attempts < 200000; ++attempts) {
        const auto cycle = model.tick();
        if (cycle.consumer.inputFire) {
            EXPECT_TRUE(cycle.bEntryHit);
            EXPECT_EQ(
                cycle.fifoHead.tag.kIndex, cycle.nextExpectedK);
            ++inputFires;
        }
        const bool bRequest = std::any_of(
            cycle.bRequest.valid.begin(), cycle.bRequest.valid.end(),
            [](bool valid) { return valid; });
        if (bRequest) {
            EXPECT_LT(cycle.bRequestBuffer, uint64_t{2});
            EXPECT_LT(
                cycle.bRequestSlot,
                config.sharedSpad.bBufferDepth);
            EXPECT_LT(cycle.bRequestK, model.derived().k);
        }
        const bool bResponse = std::any_of(
            cycle.bResponse.valid.begin(), cycle.bResponse.valid.end(),
            [](bool valid) { return valid; });
        if (bResponse) {
            EXPECT_LT(cycle.bResponseBuffer, uint64_t{2});
            EXPECT_LT(
                cycle.bResponseSlot,
                config.sharedSpad.bBufferDepth);
            EXPECT_LT(cycle.bResponseK, model.derived().k);
        }
        if (cycle.drained) {
            break;
        }
    }
    ASSERT_TRUE(model.hasDrained());
    EXPECT_EQ(model.outputs(), directOracle(config));
    EXPECT_EQ(
        inputFires, model.derived().im2col.expectedVectors);
    EXPECT_EQ(
        model.stats().bBufferFillVectors,
        model.derived().im2col.expectedVectors);
    EXPECT_EQ(
        model.stats().spadReadResponsesB,
        model.derived().im2col.expectedVectors * config.outChannels);
    EXPECT_EQ(model.stats().weightReuseHits, uint64_t{0});
    EXPECT_GT(model.stats().bBufferSwitches, uint64_t{0});
    EXPECT_GT(model.stats().bBufferEmptyCycles, uint64_t{0});
}

TEST(StreamingConvPipeline, HalfKResidentBuffersSwitchWithoutInputBubble)
{
    auto config = streamingConfig();
    config.im2col.c = 2;
    config.im2col.h = 3;
    config.im2col.w = 32;
    config.im2col.outH = 1;
    config.im2col.outW = 32;
    config.im2col.strideH = config.im2col.strideW = 1;
    config.im2col.padTop = config.im2col.padLeft = 0;
    config.sharedSpad =
        validateStreamingConfig(config).sharedSpad;
    config.sharedSpad.configured = true;
    config.sharedSpad.bBufferDepth = 9;
    config.sharedSpad.weightReuse = true;

    StreamingConvPipelineModel model(config);
    std::optional<uint64_t> previousInputCycle;
    uint64_t previousTile = 0;
    uint64_t inputFires = 0;
    for (uint64_t attempts = 0; attempts < 200000; ++attempts) {
        const auto cycle = model.tick();
        if (cycle.consumer.inputFire) {
            if (previousInputCycle &&
                cycle.fifoHead.tag.tileIndex == previousTile) {
                EXPECT_EQ(cycle.cycle, *previousInputCycle + 1);
            }
            previousInputCycle = cycle.cycle;
            previousTile = cycle.fifoHead.tag.tileIndex;
            ++inputFires;
        }
        if (cycle.drained) {
            break;
        }
    }
    ASSERT_TRUE(model.hasDrained());
    EXPECT_EQ(model.outputs(), directOracle(config));
    EXPECT_EQ(
        inputFires, model.derived().im2col.expectedVectors);
    EXPECT_EQ(model.stats().bBufferFillVectors, model.derived().k);
    EXPECT_EQ(
        model.stats().spadReadResponsesB,
        model.derived().k * config.outChannels);
    EXPECT_EQ(
        model.stats().weightReuseHits,
        (model.derived().expectedTiles - 1) * model.derived().k);
    EXPECT_EQ(
        model.stats().bBufferSwitches,
        model.derived().expectedTiles);
    EXPECT_EQ(model.stats().bBufferEmptyCycles, uint64_t{0});
}

} // anonymous namespace
} // namespace gem5::sau_n
