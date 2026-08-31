#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "sau_n/im2col_address.hh"
#include "sau_n/pipelined_im2col_model.hh"

namespace gem5::sau_n
{
namespace
{

PipelineResolvedConfig
streamingConfig()
{
    PipelineResolvedConfig config;
    config.name = "pipelined_im2col";
    config.im2col.name = "pipelined_im2col_input";
    config.im2col.n = 1;
    config.im2col.c = 1;
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
    config.outChannels = 4;
    config.cutbit = 8;
    return config;
}

std::vector<PipelinedIm2ColCycle>
runToDrained(PipelinedIm2ColModel &model, bool fifoPushReady = true)
{
    std::vector<PipelinedIm2ColCycle> cycles;
    for (uint64_t attempts = 0; attempts < 100000; ++attempts) {
        cycles.push_back(model.tick(fifoPushReady));
        if (cycles.back().drained) {
            return cycles;
        }
    }
    throw std::runtime_error("pipelined Im2Col test did not drain");
}

std::vector<PipelinedIm2ColCycle>
runSharedToDrained(
    PipelinedIm2ColModel &model,
    const BankedScratchpad &scratchpad,
    const SharedAGrantFunction &grantFunction =
        [](const SramRequest &request) { return request; })
{
    std::vector<PipelinedIm2ColCycle> cycles;
    SramRequest inFlight;
    for (uint64_t attempts = 0; attempts < 100000; ++attempts) {
        const auto response =
            scratchpad.combinationalResponse(inFlight);
        cycles.push_back(
            model.tickShared(true, response, grantFunction));
        inFlight = cycles.back().grant;
        if (cycles.back().drained) {
            EXPECT_FALSE(model.hasPendingSharedRead());
            EXPECT_TRUE(std::none_of(
                inFlight.valid.begin(), inFlight.valid.end(),
                [](bool valid) { return valid; }));
            return cycles;
        }
    }
    throw std::runtime_error("shared Im2Col test did not drain");
}

std::vector<StreamingFifoEntry>
pushedEntries(const std::vector<PipelinedIm2ColCycle> &cycles)
{
    std::vector<StreamingFifoEntry> entries;
    for (const auto &cycle : cycles) {
        if (cycle.s2Fire) {
            entries.push_back(cycle.output);
        }
    }
    return entries;
}

void
expectEntryMatchesGenerator(
    const PipelineResolvedConfig &config,
    const StreamingFifoEntry &entry)
{
    for (uint64_t row = 0; row < entry.payload.validRows; ++row) {
        const auto &coordinate = entry.payload.coordinates[row];
        ASSERT_TRUE(coordinate.valid);
        const uint64_t paddedH =
            coordinate.oh * config.im2col.strideH + entry.tag.kh;
        const uint64_t paddedW =
            coordinate.ow * config.im2col.strideW + entry.tag.kw;
        const bool padding = paddedH < config.im2col.padTop ||
            paddedW < config.im2col.padLeft ||
            paddedH - config.im2col.padTop >= config.im2col.h ||
            paddedW - config.im2col.padLeft >= config.im2col.w;
        const uint8_t expected = padding ? 0 : tbActValueV1(
            coordinate.n, entry.tag.c,
            paddedH - config.im2col.padTop,
            paddedW - config.im2col.padLeft);
        EXPECT_EQ(entry.payload.activations[row], expected);
    }
}

TEST(PipelinedIm2Col, FillsInThreeStagesThenPushesAtIiOne)
{
    auto config = streamingConfig();
    config.im2col.h = 3;
    config.im2col.w = 32;
    config.im2col.outH = 3;
    config.im2col.outW = 32;
    config.im2col.strideH = config.im2col.strideW = 1;
    PipelinedIm2ColModel model(config);

    const auto cycles = runToDrained(model);
    const auto entries = pushedEntries(cycles);
    ASSERT_EQ(entries.size(), model.derived().im2col.expectedVectors);
    ASSERT_GE(cycles.size(), 4U);
    EXPECT_TRUE(cycles[0].producerFire);
    EXPECT_FALSE(cycles[0].s0Valid);
    EXPECT_FALSE(cycles[0].s2Fire);
    EXPECT_TRUE(cycles[1].s0Fire);
    EXPECT_FALSE(cycles[1].s1Fire);
    EXPECT_TRUE(cycles[2].s0Fire);
    EXPECT_TRUE(cycles[2].s1Fire);
    EXPECT_FALSE(cycles[2].s2Fire);
    EXPECT_TRUE(cycles[3].producerFire);
    EXPECT_TRUE(cycles[3].s0Fire);
    EXPECT_TRUE(cycles[3].s1Fire);
    EXPECT_TRUE(cycles[3].s2Fire);

    uint64_t previousPush = 0;
    bool havePrevious = false;
    for (const auto &cycle : cycles) {
        if (!cycle.s2Fire) {
            continue;
        }
        if (havePrevious) {
            EXPECT_EQ(cycle.cycle, previousPush + 1);
        }
        previousPush = cycle.cycle;
        havePrevious = true;
    }
    EXPECT_EQ(cycles.size(), entries.size() + 3);
    EXPECT_EQ(model.stats().bankConflictVectors, uint64_t{0});
    EXPECT_EQ(model.stats().pipelineFillCycles, uint64_t{4});
    EXPECT_EQ(
        model.stats().producerInputPairs,
        model.stats().inputVectors - 1);
    EXPECT_EQ(
        model.stats().producerInputGapCycles,
        model.stats().producerInputPairs);
    EXPECT_EQ(
        model.stats().im2colOutputPairs,
        model.stats().outputVectors - 1);
    EXPECT_EQ(
        model.stats().im2colOutputGapCycles,
        model.stats().im2colOutputPairs);
    EXPECT_EQ(
        model.stats().conflictFreeOutputPairs,
        model.stats().im2colOutputPairs);
    EXPECT_EQ(
        model.stats().conflictFreeOutputGapCycles,
        model.stats().conflictFreeOutputPairs);
    EXPECT_EQ(model.stats().conflictFreeOutputMaxGap, uint64_t{1});
}

TEST(PipelinedIm2Col, CompactsW6Stride2ScatteredRawLanes)
{
    const auto config = streamingConfig();
    PipelinedIm2ColModel model(config);
    const auto cycles = runToDrained(model);
    const auto entries = pushedEntries(cycles);

    ASSERT_EQ(entries.size(), 9U);
    const std::array<uint8_t, 6> sources = {0, 1, 2, 6, 7, 8};
    for (uint64_t k = 0; k < entries.size(); ++k) {
        const auto &entry = entries[k];
        EXPECT_EQ(entry.tag.kIndex, k);
        EXPECT_EQ(entry.payload.validRows, uint64_t{6});
        EXPECT_EQ(entry.payload.spatialMask, uint16_t{0x003f});
        for (uint64_t row = 0; row < sources.size(); ++row) {
            EXPECT_EQ(entry.payload.sourceLanes[row], sources[row]);
            EXPECT_EQ(entry.payload.coordinates[row].oh, row / 3);
            EXPECT_EQ(entry.payload.coordinates[row].ow, row % 3);
        }
        expectEntryMatchesGenerator(config, entry);
    }
    EXPECT_EQ(model.stats().rawScatteredMaskVectors, uint64_t{9});
    EXPECT_EQ(model.stats().compactedSpatialVectors, uint64_t{9});
    EXPECT_EQ(model.stats().bankConflictVectors, uint64_t{6});
    EXPECT_EQ(model.stats().bankConflictExtraRounds, uint64_t{6});
    EXPECT_EQ(
        model.stats().bankConflictExtraRounds,
        model.stats().bankConflictStallCycles);
}

TEST(PipelinedIm2Col, CountsAndHoldsOneRoundForZeroRequestVectors)
{
    auto config = streamingConfig();
    config.im2col.h = config.im2col.w = 1;
    config.im2col.outH = config.im2col.outW = 1;
    config.im2col.strideH = config.im2col.strideW = 1;
    config.im2col.padTop = config.im2col.padLeft = 1;
    PipelinedIm2ColModel model(config);

    bool observedHeldZeroRequest = false;
    for (uint64_t attempts = 0; attempts < 100; ++attempts) {
        const auto cycle = model.tick(false);
        const bool anySramRequest = std::any_of(
            cycle.request.valid.begin(), cycle.request.valid.end(),
            [](bool valid) { return valid; });
        if (cycle.s1Valid && cycle.s1CanRetire &&
            !anySramRequest && cycle.s1.completedReadRounds == 1) {
            const auto held = model.tick(false);
            EXPECT_TRUE(held.s1Valid);
            EXPECT_TRUE(held.s1CanRetire);
            EXPECT_EQ(held.s1.tag, cycle.s1.tag);
            EXPECT_EQ(held.s1.completedReadRounds, uint64_t{1});
            EXPECT_TRUE(std::none_of(
                held.request.valid.begin(), held.request.valid.end(),
                [](bool valid) { return valid; }));
            observedHeldZeroRequest = true;
            break;
        }
    }
    EXPECT_TRUE(observedHeldZeroRequest);

    const auto recovered = runToDrained(model);
    EXPECT_TRUE(recovered.back().drained);
    EXPECT_EQ(model.stats().bankConflictVectors, uint64_t{0});
    EXPECT_EQ(model.stats().bankConflictExtraRounds, uint64_t{0});
}

TEST(PipelinedIm2Col, HoldsS2AndBackpressuresEveryUpstreamStage)
{
    auto config = streamingConfig();
    config.im2col.c = 2;
    PipelinedIm2ColModel model(config);

    StreamingFifoEntry held;
    bool foundS2 = false;
    for (uint64_t attempts = 0; attempts < 100; ++attempts) {
        const auto cycle = model.tick(false);
        if (cycle.s2Valid) {
            held = cycle.output;
            foundS2 = true;
            break;
        }
    }
    ASSERT_TRUE(foundS2);

    bool allStagesBlocked = false;
    for (uint64_t attempts = 0; attempts < 10; ++attempts) {
        const auto stalled = model.tick(false);
        ASSERT_TRUE(stalled.s2Valid);
        EXPECT_FALSE(stalled.s2Ready);
        EXPECT_FALSE(stalled.s2Fire);
        EXPECT_EQ(stalled.output.tag, held.tag);
        EXPECT_EQ(stalled.output.payload, held.payload);
        if (stalled.s0Valid && stalled.s1Valid &&
            !stalled.s0Ready && !stalled.s1Ready) {
            allStagesBlocked = true;
            break;
        }
    }
    EXPECT_TRUE(allStagesBlocked);
    EXPECT_GT(model.stats().s2StallCycles, uint64_t{0});
    EXPECT_GT(model.stats().s0StallCycles, uint64_t{0});

    const auto recovered = runToDrained(model);
    EXPECT_EQ(
        model.stats().outputVectors,
        model.derived().im2col.expectedVectors);
    EXPECT_TRUE(recovered.back().drained);
}

TEST(PipelinedIm2Col, ExcludesBackpressuredPairsFromConflictFreeIi)
{
    auto config = streamingConfig();
    config.im2col.h = 3;
    config.im2col.w = 32;
    config.im2col.outH = 1;
    config.im2col.outW = 32;
    config.im2col.strideH = config.im2col.strideW = 1;
    PipelinedIm2ColModel model(config);

    bool sawFirstPush = false;
    bool sawStall = false;
    for (uint64_t attempts = 0; attempts < 100; ++attempts) {
        const auto cycle = model.tick(!sawFirstPush || sawStall);
        sawFirstPush = sawFirstPush || cycle.s2Fire;
        if (sawFirstPush && cycle.s2Valid && !cycle.s2Ready) {
            sawStall = true;
            break;
        }
    }
    ASSERT_TRUE(sawFirstPush);
    ASSERT_TRUE(sawStall);
    runToDrained(model);

    EXPECT_LT(
        model.stats().conflictFreeOutputPairs,
        model.stats().im2colOutputPairs);
    EXPECT_EQ(
        model.stats().conflictFreeOutputGapCycles,
        model.stats().conflictFreeOutputPairs);
    EXPECT_EQ(model.stats().conflictFreeOutputMaxGap, uint64_t{1});
}

TEST(PipelinedIm2Col, PreservesCanonicalOrderAcrossTilesAndChannels)
{
    auto config = streamingConfig();
    config.im2col.n = 2;
    config.im2col.c = 2;
    config.im2col.h = 3;
    config.im2col.w = 17;
    config.im2col.outH = 2;
    config.im2col.outW = 17;
    config.im2col.strideH = config.im2col.strideW = 1;
    PipelinedIm2ColModel model(config);
    const auto entries = pushedEntries(runToDrained(model));

    ASSERT_EQ(entries.size(), model.derived().im2col.expectedVectors);
    for (uint64_t index = 0; index < entries.size(); ++index) {
        const auto &entry = entries[index];
        EXPECT_EQ(entry.tag.tileIndex, index / model.derived().k);
        EXPECT_EQ(entry.tag.kIndex, index % model.derived().k);
        expectEntryMatchesGenerator(config, entry);
    }
    EXPECT_EQ(model.stats().inputVectors, entries.size());
    EXPECT_EQ(model.stats().outputVectors, entries.size());
    EXPECT_TRUE(model.hasDrained());
    EXPECT_THROW(model.tick(true), std::logic_error);
}

TEST(PipelinedIm2Col, SupportsMaximumChannelCountAndK)
{
    auto config = streamingConfig();
    config.im2col.c = SauMaxChannels;
    config.im2col.h = config.im2col.w = 3;
    config.im2col.outH = config.im2col.outW = 1;
    config.im2col.strideH = config.im2col.strideW = 1;
    config.im2col.padTop = config.im2col.padLeft = 0;
    PipelinedIm2ColModel model(config);
    const auto entries = pushedEntries(runToDrained(model));

    ASSERT_EQ(entries.size(), uint64_t{567});
    EXPECT_TRUE(entries.front().tag.tileFirst);
    EXPECT_EQ(entries.back().tag.c, uint64_t{62});
    EXPECT_EQ(entries.back().tag.kh, uint64_t{2});
    EXPECT_EQ(entries.back().tag.kw, uint64_t{2});
    EXPECT_EQ(entries.back().tag.kIndex, uint64_t{566});
    EXPECT_TRUE(entries.back().tag.tileLast);
    EXPECT_EQ(model.stats().inputVectors, uint64_t{567});
    EXPECT_EQ(model.stats().outputVectors, uint64_t{567});
}

TEST(PipelinedIm2Col, UsesCallerProvidedScratchpad)
{
    auto config = streamingConfig();
    config.im2col.h = 3;
    config.im2col.w = 5;
    config.im2col.outH = 1;
    config.im2col.outW = 3;
    config.im2col.strideH = config.im2col.strideW = 1;
    config.im2col.padTop = config.im2col.padLeft = 0;

    BankedScratchpad scratchpad;
    scratchpad.preload(config.im2col);
    const ChwAddressMapper mapper(config.im2col);
    const auto first = mapper.locate(0, 0, 0, 0);
    scratchpad.write(first.bank, first.row, 0xee);

    PipelinedIm2ColModel model(config, scratchpad);
    const auto entries = pushedEntries(runToDrained(model));
    ASSERT_EQ(entries.size(), 9U);
    EXPECT_EQ(entries[0].tag.kIndex, uint64_t{0});
    EXPECT_EQ(entries[0].payload.activations[0], uint8_t{0xee});
}

TEST(PipelinedIm2Col, SharedOneCycleMatchesStandaloneAndTurnsOverAtIiOne)
{
    auto config = streamingConfig();
    config.im2col.c = 8;
    config.im2col.h = 3;
    config.im2col.w = 32;
    config.im2col.outH = 1;
    config.im2col.outW = 32;
    config.im2col.strideH = config.im2col.strideW = 1;
    config.im2col.padTop = config.im2col.padLeft = 0;

    BankedScratchpad scratchpad;
    scratchpad.preload(config.im2col);
    PipelinedIm2ColModel standalone(config, scratchpad);
    const auto standaloneCycles = runToDrained(standalone);
    PipelinedIm2ColModel shared(
        config, PipelinedIm2ColMemoryMode::SharedOneCycle);
    const auto sharedCycles = runSharedToDrained(shared, scratchpad);

    const auto standaloneEntries = pushedEntries(standaloneCycles);
    const auto sharedEntries = pushedEntries(sharedCycles);
    ASSERT_EQ(sharedEntries.size(), standaloneEntries.size());
    for (std::size_t index = 0; index < sharedEntries.size(); ++index) {
        EXPECT_EQ(sharedEntries[index].tag, standaloneEntries[index].tag);
        EXPECT_EQ(
            sharedEntries[index].payload,
            standaloneEntries[index].payload);
    }
    EXPECT_EQ(
        shared.stats().outputVectors,
        shared.derived().im2col.expectedVectors);
    EXPECT_EQ(
        shared.stats().conflictFreeOutputMaxGap, uint64_t{1});

    uint64_t consecutivePushes = 0;
    uint64_t longestRun = 0;
    for (const auto &cycle : sharedCycles) {
        if (cycle.s2Fire) {
            ++consecutivePushes;
            longestRun = std::max(longestRun, consecutivePushes);
        } else {
            consecutivePushes = 0;
        }
        if (!cycle.s1Fire) {
            for (uint64_t bank = 0; bank < SpBanks; ++bank) {
                if (cycle.response.valid[bank] &&
                    cycle.request.valid[bank]) {
                    EXPECT_NE(
                        cycle.request.address[bank],
                        cycle.responseRequest.address[bank]);
                }
            }
        }
    }
    EXPECT_GT(longestRun, StreamingFifoDepth + 2);
}

TEST(PipelinedIm2Col, SharedGrantDenialRetriesWithoutPhantomResponse)
{
    auto config = streamingConfig();
    config.im2col.h = 3;
    config.im2col.w = 16;
    config.im2col.outH = 1;
    config.im2col.outW = 16;
    config.im2col.strideH = config.im2col.strideW = 1;
    config.im2col.padTop = config.im2col.padLeft = 0;

    BankedScratchpad scratchpad;
    scratchpad.preload(config.im2col);
    bool deniedOnce = false;
    const SharedAGrantFunction grant =
        [&deniedOnce](const SramRequest &request) {
            SramRequest result = request;
            if (!deniedOnce && request.valid[0]) {
                result.valid[0] = false;
                deniedOnce = true;
            }
            return result;
        };
    PipelinedIm2ColModel shared(
        config, PipelinedIm2ColMemoryMode::SharedOneCycle);
    const auto cycles = runSharedToDrained(shared, scratchpad, grant);

    EXPECT_TRUE(deniedOnce);
    EXPECT_EQ(
        pushedEntries(cycles).size(),
        shared.derived().im2col.expectedVectors);
    bool sawRetry = false;
    for (std::size_t index = 1; index < cycles.size(); ++index) {
        if (!cycles[index - 1].request.valid[0] ||
            cycles[index - 1].grant.valid[0]) {
            continue;
        }
        EXPECT_FALSE(cycles[index].response.valid[0]);
        EXPECT_TRUE(cycles[index].request.valid[0]);
        sawRetry = true;
        break;
    }
    EXPECT_TRUE(sawRetry);
}

} // anonymous namespace
} // namespace gem5::sau_n
