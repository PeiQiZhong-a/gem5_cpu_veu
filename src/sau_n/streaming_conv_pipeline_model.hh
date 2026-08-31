#ifndef __SAU_N_STREAMING_CONV_PIPELINE_MODEL_HH__
#define __SAU_N_STREAMING_CONV_PIPELINE_MODEL_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "sau_n/pipelined_im2col_model.hh"
#include "sau_n/sau_model.hh"

namespace gem5::sau_n
{

struct StreamingConvPipelineStats
{
    uint64_t fifoOccupancySamples = 0;
    uint64_t fifoOccupancySum = 0;
    uint64_t fifoPeakOccupancy = 0;
    uint64_t fifoFullCycles = 0;
    uint64_t fifoPushes = 0;
    uint64_t fifoPops = 0;
    uint64_t peLaunches = 0;
    uint64_t peInputCycles = 0;
    uint64_t peInputBubbleCycles = 0;
    uint64_t peBusyNotAcceptingCycles = 0;
    uint64_t tilesGenerated = 0;
    uint64_t tilesLaunched = 0;
    uint64_t tilesCompleted = 0;
    uint64_t outputRows = 0;
    uint64_t outputElements = 0;
    uint64_t spadReadRequestsA = 0;
    uint64_t spadReadGrantsA = 0;
    uint64_t spadReadResponsesA = 0;
    uint64_t spadReadRequestsC = 0;
    uint64_t spadReadGrantsC = 0;
    uint64_t spadReadResponsesC = 0;
    uint64_t spadReadRequestsB = 0;
    uint64_t spadReadGrantsB = 0;
    uint64_t spadReadResponsesB = 0;
    uint64_t bBufferFillVectors = 0;
    uint64_t bBufferConsumedVectors = 0;
    uint64_t bBufferHitVectors = 0;
    uint64_t bBufferEmptyCycles = 0;
    uint64_t bBufferSwitches = 0;
    uint64_t bPrefetchStallCycles = 0;
    uint64_t weightReuseHits = 0;
    uint64_t spadWriteRequestsD = 0;
    uint64_t spadWriteGrantsD = 0;
    uint64_t dPendingPeak = 0;
    uint64_t dWriteStallCycles = 0;
    uint64_t bBufferOccupancySamples = 0;
    uint64_t bBufferOccupancySum = 0;
    uint64_t bBufferPeakOccupancy = 0;
    std::array<uint64_t, ScratchpadBanks> perBankReadCycles{};
    std::array<uint64_t, ScratchpadBanks> perBankWriteCycles{};
    std::array<uint64_t, ScratchpadBanks> perBankReadWriteConflicts{};
};

struct StreamingConvPipelineCycle
{
    uint64_t cycle = 0;
    StreamingConsumerState consumerState = StreamingConsumerState::Idle;
    uint64_t acceptedK = 0;
    uint64_t activeTile = 0;
    uint64_t fifoCount = 0;
    uint64_t fifoReadPointer = 0;
    uint64_t fifoWritePointer = 0;
    bool fifoHeadValid = false;
    StreamingFifoEntry fifoHead{};
    bool fifoPushReady = false;
    bool fifoPush = false;
    bool fifoPop = false;
    SramRequest bRequest{};
    SramRequest bGrant{};
    SramResponse bResponse{};
    uint64_t bRequestBuffer = 0;
    uint64_t bRequestSlot = 0;
    uint64_t bRequestK = 0;
    uint64_t bResponseBuffer = 0;
    uint64_t bResponseSlot = 0;
    uint64_t bResponseK = 0;
    SramRequest cRequest{};
    SramRequest cGrant{};
    SramResponse cResponse{};
    uint64_t cRequestByte = 0;
    uint64_t cResponseByte = 0;
    SramRequest dRequest{};
    SramRequest dGrant{};
    uint64_t dQueueOccupancy = 0;
    uint16_t dHeadPendingMask = 0;
    bool dHeadWillRetire = false;
    bool dEnqueue = false;
    bool dDequeue = false;
    bool bEntryHit = false;
    bool bReuseHit = false;
    uint64_t activeBBuffer = 0;
    uint64_t nextExpectedK = 0;
    uint64_t bReadyEntries = 0;
    StreamingConsumerDecision consumer{};
    PipelinedIm2ColCycle producer{};
    SauCycleInputs sauInputs{};
    SauCycleObservation sau{};
    bool outputCollected = false;
    bool drained = false;
};

class StreamingConvPipelineModel
{
  public:
    static constexpr SauInputProtocol InputProtocol =
        SauInputProtocol::ElasticBubbleEnabled;

    explicit StreamingConvPipelineModel(
        const PipelineResolvedConfig &config,
        const OutputReadyConfig &ready = {});
    StreamingConvPipelineModel(
        const PipelineResolvedConfig &config,
        ScratchpadBacking &backing,
        const OutputReadyConfig &ready = {});

    StreamingConvPipelineCycle tick();

    const PipelineResolvedConfig &config() const { return resolved; }
    const PipelineDerivedConfig &derived() const { return dimensions; }
    const StreamingConvPipelineStats &stats() const { return counters; }
    const PipelinedIm2ColStats &producerStats() const
    {
        return producer.stats();
    }
    PipelinedIm2ColMemoryMode producerMemoryMode() const
    {
        return producer.memoryMode();
    }
    const std::vector<int8_t> &outputs() const { return collectedOutputs; }
    const BankedScratchpad &sharedSpad() const { return sharedScratchpad; }
    uint64_t nextCycle() const { return cycleNumber; }
    bool hasDrained() const { return drainedAt.has_value(); }
    std::optional<uint64_t> drainedCycle() const { return drainedAt; }

  private:
    struct BBufferEntry
    {
        bool valid = false;
        uint64_t weightTile = 0;
        uint64_t globalK = 0;
        uint16_t readyMask = 0;
        std::array<int8_t, SauColumns> weights{};
    };

    struct BBuffer
    {
        bool valid = false;
        uint64_t weightTile = 0;
        uint64_t chunkBaseK = 0;
        uint64_t chunkLength = 0;
        std::vector<BBufferEntry> entries;
    };

    struct BReadTag
    {
        bool valid = false;
        uint64_t buffer = 0;
        uint64_t slot = 0;
        uint64_t globalK = 0;
    };

    struct BRequestContext
    {
        bool valid = false;
        uint64_t buffer = 0;
        uint64_t slot = 0;
        uint64_t globalK = 0;
        SramRequest request{};
    };

    struct CReadTag
    {
        bool valid = false;
        uint8_t byte = 0;
    };

    struct CRequestContext
    {
        SramRequest request{};
        std::array<uint8_t, SauColumns> bytes{};
    };

    struct DPendingRow
    {
        StreamingSpatialCoordinate coordinate{};
        uint16_t validMask = 0;
        uint16_t pendingMask = 0;
        std::array<int8_t, SauColumns> values{};
    };

    SauCycleInputs buildSauInputs(
        const StreamingConsumerDecision &decision,
        const StreamingFifoEntry &head, bool outputGrant) const;
    bool biasesReady() const;
    void applyCResponses(
        const SramResponse &response);
    CRequestContext buildCRequest() const;
    void installCInFlight(
        const CRequestContext &context, const SramRequest &grant);
    SramRequest buildDRequest() const;
    void applyDWrites(
        std::deque<DPendingRow> &working, const SramRequest &grant);
    void enqueueDOutput(
        std::deque<DPendingRow> &working,
        const SauCycleObservation &observation);
    void rebuildOutputsFromD();
    void completeActiveTile();
    uint64_t outputIndex(
        const StreamingSpatialCoordinate &coordinate,
        uint64_t outputChannel) const;
    void checkInvariants() const;
    void initializeModel(bool preload);
    void preloadSharedScratchpad();
    void initializeBBuffers();
    void configureBChunk(uint64_t buffer, uint64_t baseK);
    uint16_t expectedBMask() const;
    bool bBufferComplete(uint64_t buffer) const;
    uint64_t bReadyEntryCount(
        const std::array<BBuffer, 2> &buffers) const;
    bool bAllResidentReady() const;
    bool bBufferCovers(uint64_t buffer, uint64_t k) const;
    bool bEntryReady(uint64_t buffer, uint64_t k) const;
    const BBufferEntry &activeBEntry(uint64_t k) const;
    void refreshActiveB();
    void switchActiveB(uint64_t nextBuffer);
    void resetBForNextTile();
    void applyBResponses(
        std::array<BBuffer, 2> &working,
        const SramResponse &response);
    BRequestContext buildBRequest(
        const std::array<BBuffer, 2> &working) const;
    void installBInFlight(
        const BRequestContext &context,
        const SramRequest &grant);
    bool bLaunchReady(const StreamingFifoEntry &head) const;
    bool bInputReady(const StreamingFifoEntry &head) const;
    void consumeBInput(uint64_t k);

    PipelineResolvedConfig resolved;
    PipelineDerivedConfig dimensions;
    OutputReadyConfig readyConfig;
    PipelinedIm2ColModel producer;
    BankedScratchpad sharedScratchpad;
    SramRequest aInFlight{};
    SramRequest spadInFlight{};
    std::array<CReadTag, ScratchpadBanks> cInFlight{};
    std::array<std::array<uint8_t, 2>, SauColumns> biasBytes{};
    std::array<uint8_t, SauColumns> biasReadyMasks{};
    SauNumericCore::BiasLanes loadedBiases{};
    std::array<BReadTag, ScratchpadBanks> bInFlight{};
    std::array<BBuffer, 2> bBuffers{};
    uint64_t activeBBuffer = 0;
    uint64_t nextExpectedK = 0;
    uint64_t nextBChunkBase = 0;
    bool fullBResident = false;
    SauCycleModel sauModel;
    std::array<StreamingFifoEntry, StreamingFifoDepth> fifo{};
    uint64_t fifoCount = 0;
    uint64_t fifoReadPointer = 0;
    uint64_t fifoWritePointer = 0;
    StreamingConsumerState consumerState = StreamingConsumerState::Idle;
    uint64_t activeTile = 0;
    uint64_t acceptedK = 0;
    uint64_t tileOutputRows = 0;
    std::optional<StreamingFifoEntry> activeEntry;
    std::deque<DPendingRow> dQueue;
    StreamingConvPipelineStats counters{};
    std::vector<int8_t> collectedOutputs;
    std::vector<bool> outputWritten;
    uint64_t cycleNumber = 0;
    std::optional<uint64_t> drainedAt;
};

} // namespace gem5::sau_n

#endif // __SAU_N_STREAMING_CONV_PIPELINE_MODEL_HH__
