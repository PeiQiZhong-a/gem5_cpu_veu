#ifndef __SAU_N_PIPELINED_IM2COL_MODEL_HH__
#define __SAU_N_PIPELINED_IM2COL_MODEL_HH__

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "sau_n/banked_scratchpad.hh"
#include "sau_n/streaming_pipeline_contract.hh"

namespace gem5::sau_n
{

enum class PipelinedIm2ColMemoryMode : uint8_t
{
    StandaloneCombinational = 0,
    SharedOneCycle = 1,
};

using SharedAGrantFunction =
    std::function<SramRequest(const SramRequest &)>;

struct PipelinedIm2ColStats
{
    uint64_t inputVectors = 0;
    uint64_t outputVectors = 0;
    uint64_t pipelineFillCycles = 0;
    uint64_t bankConflictVectors = 0;
    uint64_t bankConflictExtraRounds = 0;
    uint64_t bankConflictStallCycles = 0;
    uint64_t rawScatteredMaskVectors = 0;
    uint64_t compactedSpatialVectors = 0;
    uint64_t producerInputPairs = 0;
    uint64_t producerInputGapCycles = 0;
    uint64_t im2colOutputPairs = 0;
    uint64_t im2colOutputGapCycles = 0;
    uint64_t conflictFreeOutputPairs = 0;
    uint64_t conflictFreeOutputGapCycles = 0;
    uint64_t conflictFreeOutputMaxGap = 0;
    uint64_t s0StallCycles = 0;
    uint64_t s1StallCycles = 0;
    uint64_t s2StallCycles = 0;
};

struct PipelinedIm2ColCycle
{
    uint64_t cycle = 0;
    bool s0Valid = false;
    bool s0Ready = false;
    bool s0Fire = false;
    bool s1Valid = false;
    bool s1CanRetire = false;
    bool s1Ready = false;
    bool s1Fire = false;
    bool s2Valid = false;
    bool s2Ready = false;
    bool s2Fire = false;
    bool producerFire = false;
    StreamingS0Payload s0{};
    StreamingS1Payload s1{};
    StreamingS2Payload s2{};
    SramRequest responseRequest{};
    SramRequest request{};
    SramRequest grant{};
    SramResponse response{};
    StreamingFifoEntry output{};
    bool producerExhausted = false;
    bool drained = false;
};

class PipelinedIm2ColModel
{
  public:
    explicit PipelinedIm2ColModel(const PipelineResolvedConfig &config);
    PipelinedIm2ColModel(
        const PipelineResolvedConfig &config,
        PipelinedIm2ColMemoryMode memoryMode);
    PipelinedIm2ColModel(
        const PipelineResolvedConfig &config,
        const BankedScratchpad &preloadedScratchpad);

    PipelinedIm2ColCycle tick(bool fifoPushReady);
    PipelinedIm2ColCycle tickShared(
        bool fifoPushReady,
        const SramResponse &previousResponse,
        const SharedAGrantFunction &grantFunction);

    const PipelineResolvedConfig &config() const { return resolved; }
    const PipelineDerivedConfig &derived() const { return dimensions; }
    const PipelinedIm2ColStats &stats() const { return counters; }
    uint64_t nextCycle() const { return cycleNumber; }
    bool hasDrained() const { return drainedAt.has_value(); }
    std::optional<uint64_t> drainedCycle() const { return drainedAt; }
    PipelinedIm2ColMemoryMode memoryMode() const { return accessMode; }
    bool hasPendingSharedRead() const;

  private:
    struct Iterators
    {
        uint64_t n = 0;
        uint64_t c = 0;
        uint64_t oh = 0;
        uint64_t owBase = 0;
        uint64_t kh = 0;
        uint64_t kw = 0;
        uint64_t tileIndex = 0;
    };

    struct Registers
    {
        bool s0Valid = false;
        StreamingS0Payload s0{};
        bool s1Valid = false;
        StreamingS1Payload s1{};
        bool s2Valid = false;
        StreamingS2Payload s2{};
        Iterators iterators{};
        bool producerExhausted = false;
    };

    StreamingS0Payload buildS0Payload(const Iterators &iterators) const;
    StreamingS1Payload beginS1(
        const StreamingS0Payload &payload) const;
    SramRequest arbitrateS1(const StreamingS1Payload &payload) const;
    StreamingS1Payload collectS1(
        const StreamingS1Payload &payload,
        const SramRequest &request,
        const SramResponse &response) const;
    bool s1Complete(const StreamingS1Payload &payload) const;
    StreamingS2Payload finishS2(
        const StreamingS1Payload &payload) const;
    bool advanceIterators(Iterators &iterators) const;
    void noteGenerated(const StreamingS0Payload &payload);
    void noteRetiredS1(const StreamingS1Payload &payload);
    void notePushed(const StreamingS2Payload &payload);
    void checkS0Payload(const StreamingS0Payload &payload) const;
    void checkS1Payload(const StreamingS1Payload &payload) const;
    void checkS2Payload(const StreamingS2Payload &payload) const;
    void checkInvariants(const Registers &value) const;
    PipelinedIm2ColCycle tickImpl(
        bool fifoPushReady,
        const SramResponse *previousResponse,
        const SharedAGrantFunction *grantFunction);
    void validateSharedGrant(
        const SramRequest &request, const SramRequest &grant) const;
    void validateSharedResponse(const SramResponse &response) const;

    PipelineResolvedConfig resolved;
    PipelineDerivedConfig dimensions;
    PipelinedIm2ColMemoryMode accessMode =
        PipelinedIm2ColMemoryMode::StandaloneCombinational;
    std::unique_ptr<BankedScratchpad> standaloneScratchpad;
    SramRequest sharedInFlight{};
    Registers registers{};
    PipelinedIm2ColStats counters{};
    uint64_t cycleNumber = 0;
    std::optional<uint64_t> drainedAt;
    std::optional<StreamingVectorTag> lastGeneratedTag;
    std::optional<StreamingVectorTag> lastPushedTag;
    std::optional<StreamingFifoEntry> lastPushedEntry;
    std::optional<uint64_t> lastGeneratedCycle;
    std::optional<uint64_t> lastPushedCycle;
    bool lastPushedConflictFree = false;
    bool downstreamStallSinceLastPush = false;
};

} // namespace gem5::sau_n

#endif // __SAU_N_PIPELINED_IM2COL_MODEL_HH__
