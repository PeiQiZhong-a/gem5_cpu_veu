#include <gtest/gtest.h>

#include <stdexcept>

#include "sau_n/streaming_pipeline_contract.hh"

namespace gem5::sau_n
{
namespace
{

PipelineResolvedConfig
streamingConfig()
{
    PipelineResolvedConfig config;
    config.name = "streaming_contract";
    config.im2col.name = "streaming_contract_im2col";
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

RawSpatialPayload
scatteredPayload()
{
    RawSpatialPayload raw;
    const std::array<uint64_t, 6> lanes = {0, 1, 2, 6, 7, 8};
    for (uint64_t index = 0; index < lanes.size(); ++index) {
        const uint64_t lane = lanes[index];
        raw.spatialMask |= uint16_t{1} << lane;
        raw.activations[lane] = static_cast<uint8_t>(10 + lane);
        raw.coordinates[lane] = {
            true,
            0,
            index / 3,
            index % 3,
        };
    }
    return raw;
}

StreamingVectorTag
tagFor(uint64_t tile, uint64_t kIndex, uint64_t k = 18)
{
    const uint64_t kernelIndex = kIndex % 9;
    return {
        tile,
        0,
        3,
        kIndex / 9,
        kernelIndex / 3,
        kernelIndex % 3,
        kIndex,
        kIndex == 0,
        kIndex + 1 == k,
    };
}

TEST(StreamingConfig, AcceptsW6Stride2AndRejectsExplorationLimits)
{
    const auto derived = validateStreamingConfig(streamingConfig());
    EXPECT_EQ(derived.k, uint64_t{18});
    EXPECT_EQ(derived.expectedTiles, uint64_t{1});
    EXPECT_EQ(derived.sharedSpad.aBase, uint64_t{0});
    EXPECT_EQ(
        derived.sharedSpad.bBase,
        derived.sharedSpad.aBase + derived.sharedSpad.aRows);
    EXPECT_EQ(
        derived.sharedSpad.cBase,
        derived.sharedSpad.bBase + derived.sharedSpad.bRows);
    EXPECT_EQ(
        derived.sharedSpad.dBase,
        derived.sharedSpad.cBase + derived.sharedSpad.cRows);
    EXPECT_EQ(derived.sharedSpad.bBufferDepth, derived.k);
    EXPECT_EQ(derived.sharedSpad.dPendingRows, uint64_t{1});
    EXPECT_TRUE(derived.sharedSpad.weightReuse);

    auto invalid = streamingConfig();
    invalid.im2col.strideW = 1;
    EXPECT_THROW(validateStreamingConfig(invalid), std::invalid_argument);
    invalid = streamingConfig();
    invalid.im2col.strideH = invalid.im2col.strideW = 3;
    EXPECT_THROW(validateStreamingConfig(invalid), std::invalid_argument);
    invalid = streamingConfig();
    invalid.im2col.padLeft = 0;
    EXPECT_THROW(validateStreamingConfig(invalid), std::invalid_argument);
    invalid = streamingConfig();
    invalid.im2col.padTop = invalid.im2col.padLeft = 2;
    EXPECT_THROW(validateStreamingConfig(invalid), std::invalid_argument);
    invalid = streamingConfig();
    invalid.im2col.dilationW = 2;
    EXPECT_THROW(validateStreamingConfig(invalid), std::invalid_argument);
}

TEST(StreamingConfig, FreezesAddressesAndRejectsInvalidRegions)
{
    const auto config = streamingConfig();
    const auto derived = validateStreamingConfig(config);
    EXPECT_EQ(
        bAddress(config, derived, 17, 2),
        (ScratchpadAddress{2, derived.sharedSpad.bBase + 17}));
    EXPECT_EQ(
        cAddress(config, derived, 2, 0),
        (ScratchpadAddress{2, derived.sharedSpad.cBase}));
    EXPECT_EQ(
        cAddress(config, derived, 2, 1),
        (ScratchpadAddress{2, derived.sharedSpad.cBase + 1}));
    EXPECT_EQ(
        dAddress(config, derived, 0, 1, 2, 2),
        (ScratchpadAddress{2, derived.sharedSpad.dBase + 5}));
    EXPECT_THROW(
        bAddress(config, derived, 18, 0), std::invalid_argument);
    EXPECT_THROW(
        cAddress(config, derived, 0, 2), std::invalid_argument);
    EXPECT_THROW(
        dAddress(config, derived, 1, 0, 0, 0),
        std::invalid_argument);

    auto invalid = config;
    invalid.sharedSpad = derived.sharedSpad;
    invalid.sharedSpad.configured = true;
    invalid.sharedSpad.bBase = invalid.sharedSpad.aBase;
    EXPECT_THROW(validateStreamingConfig(invalid), std::invalid_argument);
    invalid.sharedSpad = derived.sharedSpad;
    invalid.sharedSpad.configured = true;
    --invalid.sharedSpad.bRows;
    EXPECT_THROW(validateStreamingConfig(invalid), std::invalid_argument);
    invalid.sharedSpad = derived.sharedSpad;
    invalid.sharedSpad.configured = true;
    invalid.sharedSpad.dBase = SpBankEntries - 1;
    EXPECT_THROW(validateStreamingConfig(invalid), std::invalid_argument);
}

TEST(StreamingCompaction, StablyCompactsScatteredRawLanes)
{
    const auto compacted = compactSpatialPayload(scatteredPayload());
    EXPECT_EQ(compacted.validRows, uint64_t{6});
    EXPECT_EQ(compacted.spatialMask, uint16_t{0x003f});
    EXPECT_TRUE(isCanonicalPrefixMask(compacted.spatialMask));
    const std::array<uint8_t, 6> expectedSources = {0, 1, 2, 6, 7, 8};
    for (uint64_t row = 0; row < expectedSources.size(); ++row) {
        EXPECT_EQ(compacted.sourceLanes[row], expectedSources[row]);
        EXPECT_EQ(
            compacted.activations[row],
            static_cast<uint8_t>(10 + expectedSources[row]));
        EXPECT_EQ(compacted.coordinates[row].oh, row / 3);
        EXPECT_EQ(compacted.coordinates[row].ow, row % 3);
    }
    for (uint64_t row = 6; row < SauRows; ++row) {
        EXPECT_EQ(compacted.activations[row], uint8_t{0});
        EXPECT_EQ(compacted.sourceLanes[row], uint8_t{0});
        EXPECT_FALSE(compacted.coordinates[row].valid);
    }
}

TEST(StreamingCompaction, RejectsNoncanonicalOrDuplicateRawMetadata)
{
    RawSpatialPayload empty;
    EXPECT_THROW(compactSpatialPayload(empty), std::invalid_argument);

    auto invalid = scatteredPayload();
    invalid.coordinates[0].valid = false;
    EXPECT_THROW(compactSpatialPayload(invalid), std::invalid_argument);
    invalid = scatteredPayload();
    invalid.activations[3] = 1;
    EXPECT_THROW(compactSpatialPayload(invalid), std::invalid_argument);
    invalid = scatteredPayload();
    invalid.coordinates[6] = invalid.coordinates[0];
    EXPECT_THROW(compactSpatialPayload(invalid), std::invalid_argument);
}

TEST(StreamingTag, EnforcesCanonicalKAndTileBoundaries)
{
    EXPECT_NO_THROW(validateVectorTag(tagFor(7, 0), 18));
    EXPECT_NO_THROW(validateVectorTag(tagFor(7, 17), 18));
    auto invalid = tagFor(7, 4);
    ++invalid.kw;
    EXPECT_THROW(validateVectorTag(invalid, 18), std::invalid_argument);
    invalid = tagFor(7, 4);
    invalid.tileFirst = true;
    EXPECT_THROW(validateVectorTag(invalid, 18), std::invalid_argument);
    invalid = tagFor(7, 4);
    invalid.ocGroup = 1;
    EXPECT_THROW(validateVectorTag(invalid, 18), std::invalid_argument);
}

TEST(StreamingTag, RequiresStableMetadataWithinTile)
{
    const auto payload = compactSpatialPayload(scatteredPayload());
    EXPECT_NO_THROW(validateSameTileMetadata(
        tagFor(1, 0), payload, tagFor(1, 1), payload));
    auto changed = payload;
    changed.sourceLanes[0] = 1;
    EXPECT_THROW(
        validateSameTileMetadata(
            tagFor(1, 0), payload, tagFor(1, 1), changed),
        std::invalid_argument);
    EXPECT_THROW(
        validateSameTileMetadata(
            tagFor(1, 0), payload, tagFor(2, 1), payload),
        std::invalid_argument);
}

TEST(StreamingTag, EnforcesContiguousVectorAndTileSequence)
{
    EXPECT_NO_THROW(validateVectorSequence(tagFor(1, 4), tagFor(1, 5), 18));
    EXPECT_NO_THROW(validateVectorSequence(tagFor(1, 17), tagFor(2, 0), 18));
    EXPECT_THROW(
        validateVectorSequence(tagFor(1, 4), tagFor(1, 6), 18),
        std::invalid_argument);
    EXPECT_THROW(
        validateVectorSequence(tagFor(1, 17), tagFor(3, 0), 18),
        std::invalid_argument);
    EXPECT_THROW(
        validateVectorSequence(tagFor(1, 4), tagFor(2, 0), 18),
        std::invalid_argument);
}

TEST(StreamingElastic, AllowsAllFourTransfersInOneCycle)
{
    const auto decision = decideElasticAdvance({
        true, true, true, true, 3, true, true,
    });
    EXPECT_TRUE(decision.fifoPushReady);
    EXPECT_TRUE(decision.s2Ready);
    EXPECT_TRUE(decision.s1Ready);
    EXPECT_TRUE(decision.s0Ready);
    EXPECT_TRUE(decision.producerReady);
    EXPECT_TRUE(decision.s2ToFifo);
    EXPECT_TRUE(decision.s1ToS2);
    EXPECT_TRUE(decision.s0ToS1);
    EXPECT_TRUE(decision.producerToS0);
}

TEST(StreamingElastic, HoldsUpstreamForConflictAndBackpressure)
{
    const auto conflict = decideElasticAdvance({
        true, true, false, false, 0, false, true,
    });
    EXPECT_FALSE(conflict.s1Ready);
    EXPECT_FALSE(conflict.s0Ready);
    EXPECT_FALSE(conflict.producerReady);

    const auto full = decideElasticAdvance({
        true, true, true, true, 4, false, true,
    });
    EXPECT_FALSE(full.fifoPushReady);
    EXPECT_FALSE(full.s2Ready);
    EXPECT_FALSE(full.s1Ready);
    EXPECT_FALSE(full.s0Ready);
}

TEST(StreamingFifo, SupportsFullPopPushAndChecksConservation)
{
    const auto exchange = decideElasticFifo(4, true, true);
    EXPECT_TRUE(exchange.pushReady);
    EXPECT_TRUE(exchange.push);
    EXPECT_TRUE(exchange.pop);
    EXPECT_EQ(exchange.nextCount, uint64_t{4});

    const auto blocked = decideElasticFifo(4, true, false);
    EXPECT_FALSE(blocked.pushReady);
    EXPECT_FALSE(blocked.push);
    EXPECT_EQ(blocked.nextCount, uint64_t{4});
    EXPECT_THROW(decideElasticFifo(0, false, true), std::logic_error);
    EXPECT_THROW(decideElasticFifo(5, false, false), std::out_of_range);
}

TEST(DPendingQueue, DepthOneRetiresAndEnqueuesInTheSameCycle)
{
    const auto exchange = decideDPendingQueue(
        1, 1, 0x0007, 0x0007, true);
    EXPECT_TRUE(exchange.headWillRetire);
    EXPECT_TRUE(exchange.pushReady);
    EXPECT_TRUE(exchange.outputGrant);
}

TEST(DPendingQueue, PartialWriteBackpressuresTheSauAtDepthOne)
{
    const auto partial = decideDPendingQueue(
        1, 1, 0x0007, 0x0003, true);
    EXPECT_FALSE(partial.headWillRetire);
    EXPECT_FALSE(partial.pushReady);
    EXPECT_FALSE(partial.outputGrant);

    const auto externallyStalled = decideDPendingQueue(
        1, 1, 0x0007, 0x0007, false);
    EXPECT_TRUE(externallyStalled.headWillRetire);
    EXPECT_TRUE(externallyStalled.pushReady);
    EXPECT_FALSE(externallyStalled.outputGrant);

    EXPECT_THROW(
        decideDPendingQueue(0, 1, 0x0001, 0, true),
        std::logic_error);
    EXPECT_THROW(
        decideDPendingQueue(1, 1, 0x0001, 0x0002, true),
        std::logic_error);
}

TEST(SharedSpadArbitration, AppliesAThenDThenBPerBank)
{
    SramRequest a;
    SramRequest b;
    SramRequest d;
    for (uint64_t bank = 0; bank < 4; ++bank) {
        b.valid[bank] = true;
        b.address[bank] = static_cast<uint16_t>(100 + bank);
    }
    a.valid[0] = true;
    a.address[0] = 10;
    a.valid[1] = true;
    a.address[1] = 11;
    d.valid[1] = true;
    d.address[1] = 21;
    d.valid[2] = true;
    d.address[2] = 22;

    const auto decision = arbitrateSharedSpad(a, b, {}, d);
    EXPECT_TRUE(decision.aGrant.valid[0]);
    EXPECT_TRUE(decision.aGrant.valid[1]);
    EXPECT_FALSE(decision.dGrant.valid[1]);
    EXPECT_TRUE(decision.dGrant.valid[2]);
    EXPECT_FALSE(decision.bGrant.valid[0]);
    EXPECT_FALSE(decision.bGrant.valid[1]);
    EXPECT_FALSE(decision.bGrant.valid[2]);
    EXPECT_TRUE(decision.bGrant.valid[3]);
    EXPECT_EQ(decision.readGrant.address[0], uint16_t{10});
    EXPECT_EQ(decision.readGrant.address[1], uint16_t{11});
    EXPECT_FALSE(decision.readGrant.valid[2]);
    EXPECT_EQ(decision.readGrant.address[3], uint16_t{103});
}

TEST(SharedSpadArbitration, KeepsCInitializationExclusive)
{
    SramRequest c;
    c.valid[3] = true;
    c.address[3] = 33;
    const auto cOnly = arbitrateSharedSpad({}, {}, c, {});
    EXPECT_TRUE(cOnly.cGrant.valid[3]);
    EXPECT_TRUE(cOnly.readGrant.valid[3]);
    EXPECT_EQ(cOnly.readGrant.address[3], uint16_t{33});

    SramRequest b;
    b.valid[4] = true;
    EXPECT_THROW(
        arbitrateSharedSpad({}, b, c, {}),
        std::logic_error);
}

TEST(StreamingConsumer, LaunchesThenAcceptsOnlyMatchingTileAndK)
{
    const auto first = tagFor(3, 0);
    const auto idle = decideStreamingConsumer(
        StreamingConsumerState::Idle, true, first, 0, 0, 18);
    EXPECT_TRUE(idle.beginLaunch);
    EXPECT_FALSE(idle.launch);
    EXPECT_FALSE(idle.inputFire);
    const auto launch = decideStreamingConsumer(
        StreamingConsumerState::Launch, true, first, 3, 0, 18);
    EXPECT_TRUE(launch.launch);

    const auto accept = decideStreamingConsumer(
        StreamingConsumerState::AcceptK, true, tagFor(3, 5), 3, 5, 18);
    EXPECT_TRUE(accept.peReady);
    EXPECT_TRUE(accept.inputValid);
    EXPECT_TRUE(accept.inputFire);
    EXPECT_THROW(
        decideStreamingConsumer(
            StreamingConsumerState::AcceptK, true, tagFor(4, 5),
            3, 5, 18),
        std::logic_error);
    EXPECT_THROW(
        decideStreamingConsumer(
            StreamingConsumerState::AcceptK, true, tagFor(3, 6),
            3, 5, 18),
        std::logic_error);
}

TEST(StreamingConsumer, DoesNotConsumeNextTileWhileBusy)
{
    for (const auto state : {
             StreamingConsumerState::WaitResult,
             StreamingConsumerState::DrainOutput,
         }) {
        const auto decision = decideStreamingConsumer(
            state, true, tagFor(4, 0), 3, 18, 18);
        EXPECT_FALSE(decision.peReady);
        EXPECT_FALSE(decision.inputValid);
        EXPECT_FALSE(decision.inputFire);
    }
}

TEST(StreamingSauInput, IsolatesStrictAndElasticBubbleProtocols)
{
    EXPECT_THROW(
        decideSauInputCycle(
            SauInputProtocol::StrictRtlContinuous,
            true, 5, 18, false, true),
        std::logic_error);

    const auto bubble = decideSauInputCycle(
        SauInputProtocol::ElasticBubbleEnabled,
        true, 5, 18, false, true);
    EXPECT_FALSE(bubble.scheduleNewMac);
    EXPECT_TRUE(bubble.commitPreviouslyScheduledMac);
    EXPECT_EQ(bubble.acceptedNext, uint64_t{5});

    const auto fire = decideSauInputCycle(
        SauInputProtocol::ElasticBubbleEnabled,
        true, 5, 18, true, false);
    EXPECT_TRUE(fire.scheduleNewMac);
    EXPECT_EQ(fire.acceptedNext, uint64_t{6});
}

TEST(StreamingConservation, RequiresEveryDrainedBoundaryToMatch)
{
    const StreamingConservationCounts complete = {
        36, 36, 36, 36, 36, 36,
        2, 2, 2, 2,
    };
    EXPECT_NO_THROW(validateDrainedConservation(complete));

    auto missingVector = complete;
    --missingVector.fifoPopped;
    EXPECT_THROW(
        validateDrainedConservation(missingVector), std::logic_error);
    auto missingTile = complete;
    --missingTile.tilesCompleted;
    EXPECT_THROW(
        validateDrainedConservation(missingTile), std::logic_error);
}

} // anonymous namespace
} // namespace gem5::sau_n
