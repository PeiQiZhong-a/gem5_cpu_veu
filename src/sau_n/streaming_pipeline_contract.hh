#ifndef __SAU_N_STREAMING_PIPELINE_CONTRACT_HH__
#define __SAU_N_STREAMING_PIPELINE_CONTRACT_HH__

#include <array>
#include <cstdint>

#include "sau_n/banked_scratchpad.hh"
#include "sau_n/sau_types.hh"

namespace gem5::sau_n
{

inline constexpr uint64_t StreamingFifoDepth = 4;

enum class StreamingConsumerState : uint8_t
{
    Idle = 0,
    Launch = 1,
    AcceptK = 2,
    WaitResult = 3,
    DrainOutput = 4,
};

enum class StreamingLaneSource : uint8_t
{
    InvalidTail = 0,
    PaddingZero = 1,
    ScratchpadRead = 2,
};

struct StreamingSpatialCoordinate
{
    bool valid = false;
    uint64_t n = 0;
    uint64_t oh = 0;
    uint64_t ow = 0;

    bool operator==(const StreamingSpatialCoordinate &other) const
    {
        return valid == other.valid && n == other.n && oh == other.oh &&
            ow == other.ow;
    }
};

struct RawSpatialPayload
{
    std::array<uint8_t, SauRows> activations{};
    uint16_t spatialMask = 0;
    std::array<StreamingSpatialCoordinate, SauRows> coordinates{};
};

struct CompactedSpatialPayload
{
    std::array<uint8_t, SauRows> activations{};
    uint16_t spatialMask = 0;
    uint64_t validRows = 0;
    std::array<uint8_t, SauRows> sourceLanes{};
    std::array<StreamingSpatialCoordinate, SauRows> coordinates{};

    bool operator==(const CompactedSpatialPayload &other) const
    {
        return activations == other.activations &&
            spatialMask == other.spatialMask &&
            validRows == other.validRows &&
            sourceLanes == other.sourceLanes &&
            coordinates == other.coordinates;
    }
};

struct StreamingVectorTag
{
    uint64_t tileIndex = 0;
    uint64_t ocGroup = 0;
    uint64_t validColumns = 0;
    uint64_t c = 0;
    uint64_t kh = 0;
    uint64_t kw = 0;
    uint64_t kIndex = 0;
    bool tileFirst = false;
    bool tileLast = false;

    bool operator==(const StreamingVectorTag &other) const
    {
        return tileIndex == other.tileIndex &&
            ocGroup == other.ocGroup &&
            validColumns == other.validColumns && c == other.c &&
            kh == other.kh && kw == other.kw &&
            kIndex == other.kIndex && tileFirst == other.tileFirst &&
            tileLast == other.tileLast;
    }
};

struct StreamingLaneRequest
{
    StreamingLaneSource source = StreamingLaneSource::InvalidTail;
    StreamingSpatialCoordinate coordinate{};
    uint64_t inputH = 0;
    uint64_t inputW = 0;
    uint64_t bank = 0;
    uint64_t row = 0;
};

struct StreamingS0Payload
{
    StreamingVectorTag tag{};
    std::array<StreamingLaneRequest, SauRows> lanes{};
};

struct StreamingS1Payload
{
    StreamingVectorTag tag{};
    std::array<StreamingLaneRequest, SauRows> requests{};
    std::array<bool, SauRows> laneDone{};
    std::array<uint8_t, SauRows> activations{};
    uint16_t rawSpatialMask = 0;
    uint64_t completedReadRounds = 0;
};

struct StreamingS2Payload
{
    StreamingVectorTag tag{};
    RawSpatialPayload raw{};
    CompactedSpatialPayload compacted{};
    uint64_t readRounds = 0;
};

struct StreamingFifoEntry
{
    StreamingVectorTag tag{};
    CompactedSpatialPayload payload{};
};

struct StreamingConservationCounts
{
    uint64_t expectedVectors = 0;
    uint64_t producerAccepted = 0;
    uint64_t s2Pushed = 0;
    uint64_t fifoPushed = 0;
    uint64_t fifoPopped = 0;
    uint64_t peAccepted = 0;
    uint64_t expectedTiles = 0;
    uint64_t tilesGenerated = 0;
    uint64_t tilesLaunched = 0;
    uint64_t tilesCompleted = 0;
};

struct ElasticAdvanceInputs
{
    bool s0Valid = false;
    bool s1Valid = false;
    bool s2Valid = false;
    bool s1CanRetire = false;
    uint64_t fifoCount = 0;
    bool fifoPop = false;
    bool moreVectors = false;
};

struct ElasticAdvanceDecision
{
    bool fifoPushReady = false;
    bool s2Ready = false;
    bool s1Ready = false;
    bool s0Ready = false;
    bool producerReady = false;
    bool s2ToFifo = false;
    bool s1ToS2 = false;
    bool s0ToS1 = false;
    bool producerToS0 = false;
};

struct ElasticFifoDecision
{
    bool pushReady = false;
    bool push = false;
    bool pop = false;
    uint64_t nextCount = 0;
};

struct DPendingQueueDecision
{
    bool headWillRetire = false;
    bool pushReady = false;
    bool outputGrant = false;
};

struct SharedSpadArbitrationDecision
{
    SramRequest aGrant{};
    SramRequest bGrant{};
    SramRequest cGrant{};
    SramRequest dGrant{};
    SramRequest readGrant{};
};

struct StreamingConsumerDecision
{
    bool beginLaunch = false;
    bool launch = false;
    bool peReady = false;
    bool inputValid = false;
    bool inputFire = false;
};

struct SauInputCycleDecision
{
    bool scheduleNewMac = false;
    bool commitPreviouslyScheduledMac = false;
    uint64_t acceptedNext = 0;
};

struct ScratchpadAddress
{
    uint64_t bank = 0;
    uint64_t row = 0;

    bool operator==(const ScratchpadAddress &other) const
    {
        return bank == other.bank && row == other.row;
    }
};

PipelineDerivedConfig validateStreamingConfig(
    const PipelineResolvedConfig &config);
SharedSpadConfig resolveSharedSpadConfig(
    const PipelineResolvedConfig &config,
    const PipelineDerivedConfig &derived);
ScratchpadAddress bAddress(
    const PipelineResolvedConfig &config,
    const PipelineDerivedConfig &derived,
    uint64_t kIndex, uint64_t outputChannel);
ScratchpadAddress cAddress(
    const PipelineResolvedConfig &config,
    const PipelineDerivedConfig &derived,
    uint64_t outputChannel, uint64_t byteIndex);
ScratchpadAddress dAddress(
    const PipelineResolvedConfig &config,
    const PipelineDerivedConfig &derived,
    uint64_t n, uint64_t oh, uint64_t ow, uint64_t outputChannel);
bool isCanonicalPrefixMask(uint16_t mask);
CompactedSpatialPayload compactSpatialPayload(
    const RawSpatialPayload &raw);
void validateVectorTag(const StreamingVectorTag &tag, uint64_t k);
void validateVectorSequence(
    const StreamingVectorTag &previousTag,
    const StreamingVectorTag &currentTag,
    uint64_t k);
void validateSameTileMetadata(
    const StreamingVectorTag &previousTag,
    const CompactedSpatialPayload &previousPayload,
    const StreamingVectorTag &currentTag,
    const CompactedSpatialPayload &currentPayload);
ElasticAdvanceDecision decideElasticAdvance(
    const ElasticAdvanceInputs &inputs);
ElasticFifoDecision decideElasticFifo(
    uint64_t count, bool pushValid, bool popRequest);
DPendingQueueDecision decideDPendingQueue(
    uint64_t occupancy, uint64_t depth, uint16_t headPendingMask,
    uint16_t writeGrantMask, bool outputReady);
SharedSpadArbitrationDecision arbitrateSharedSpad(
    const SramRequest &aRequest,
    const SramRequest &bRequest,
    const SramRequest &cRequest,
    const SramRequest &dRequest);
StreamingConsumerDecision decideStreamingConsumer(
    StreamingConsumerState state,
    bool fifoValid,
    const StreamingVectorTag &headTag,
    uint64_t activeTile,
    uint64_t acceptedK,
    uint64_t k);
SauInputCycleDecision decideSauInputCycle(
    SauInputProtocol protocol,
    bool streamActive,
    uint64_t acceptedInputs,
    uint64_t calcCycles,
    bool inputFire,
    bool previouslyScheduledMacDue);
void validateDrainedConservation(
    const StreamingConservationCounts &counts);

} // namespace gem5::sau_n

#endif // __SAU_N_STREAMING_PIPELINE_CONTRACT_HH__
