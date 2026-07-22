#ifndef __BRS_VEU_TIMING_VEU_HH__
#define __BRS_VEU_TIMING_VEU_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

#include "base/types.hh"
#include "brs/veu/veu_endpoint.hh"
#include "brs/veu/veu_functional.hh"
#include "brs/veu/veu_timing_profile.hh"

namespace gem5
{
namespace brs
{

struct VeuTimingConfig
{
    uint32_t inputFifoDepth = 4;
    uint32_t executeLatency = VeuComputeDelayCycles;
    uint32_t executeII = 1;
    uint32_t vsuLatency = 1;
    uint32_t maxOutstandingReads = 4;
    uint32_t startupCycles = 0;
    uint32_t finishCycles = 0;
    std::string timingProfilePath;
    std::string cycleTracePath;
};

struct TimingVeuMemoryRequest
{
    uint64_t transactionId = 0;
    uint64_t operationId = 0;
    uint32_t chunkIndex = 0;
    VeuSource source = VeuSource::None;
    Addr address = 0;
    bool isWrite = false;
    uint32_t writeStrobe = 0xffffffffu;
    VeuVector data = {};
};

class TimingVeu : public VeuEndpoint
{
  public:
    enum class State : uint8_t
    {
        Idle,
        Running,
        Draining
    };

    using MemoryRequestFn =
        std::function<bool(const TimingVeuMemoryRequest &)>;

    TimingVeu();

    void reset() override;
    VeuResponse evaluate() const override;
    void clock(const VeuRequest &request) override;

    void configure(const VeuTimingConfig &config);
    void setMemoryRequestCallback(MemoryRequestFn callback);
    void completeMemoryRead(uint64_t transactionId, const VeuVector &data);
    void completeMemoryWrite(uint64_t transactionId);
    void noteMemoryRetry() { ++retries; }

    State state() const { return currentState; }
    bool operationBusy() const { return internalActive; }
    bool lockIsActive() const { return lockActive; }
    bool statusIsBusy() const { return statusBusy; }
    bool quiescent() const;

    uint64_t acceptedRequestCount() const { return acceptedRequests; }
    uint64_t responseCount() const { return responses; }
    uint64_t startedOperationCount() const { return startedOperations; }
    uint64_t completedOperationCount() const { return completedOperations; }
    uint64_t chunkCount() const { return processedChunks; }
    uint64_t busyCycleCount() const { return busyCycles; }
    uint64_t loadWaitCycleCount() const { return loadWaitCycles; }
    uint64_t executeCycleCount() const { return executeCycles; }
    uint64_t storeWaitCycleCount() const { return storeWaitCycles; }
    uint64_t memoryReadCount() const { return memoryReads; }
    uint64_t memoryWriteCount() const { return memoryWrites; }
    uint64_t statusActiveCycleCount() const { return statusActiveCycles; }
    uint64_t lockActiveCycleCount() const { return lockActiveCycles; }
    uint64_t currentOutstandingReadCount() const;
    uint64_t maxOutstandingReadCount() const { return maxOutstandingReadsSeen; }
    uint64_t fifoMaxOccupancy(unsigned source) const;
    uint64_t fifoFullStallCount() const { return fifoFullStalls; }
    uint64_t fifoEmptyStallCount() const { return fifoEmptyStalls; }
    uint64_t vfuAcceptedCount() const { return vfuAccepted; }
    uint64_t vfuCompletedCount() const { return vfuCompleted; }
    uint64_t maxVfuInFlightCount() const { return maxVfuInFlight; }
    uint64_t vfuIIStallCount() const { return vfuIIStalls; }
    uint64_t vsuQueueStallCount() const { return vsuQueueStalls; }
    uint64_t storePriorityCount() const { return storePriorityCycles; }
    uint64_t readBlockedByStoreCount() const { return readsBlockedByStore; }
    uint64_t maskedWriteCount() const { return maskedWrites; }
    uint64_t zeroMaskSkippedWriteCount() const { return zeroMaskSkippedWrites; }
    uint64_t retryCount() const { return retries; }
    uint64_t unexpectedResponseCount() const { return unexpectedResponses; }
    uint64_t profileHitCount() const { return profileHits; }
    uint64_t profileMissCount() const { return profileMisses; }
    uint64_t profileFallbackCount() const { return profileFallbacks; }
    uint64_t zeroLengthNoopCount() const { return zeroLengthNoops; }
    uint64_t illegalOperationCount() const { return illegalOperations; }

    uint32_t csrStatus() const { return csr.status; }
    uint32_t csrReadAddress1() const { return csr.raddr1; }
    uint32_t csrReadAddress2() const { return csr.raddr2; }
    uint32_t csrReadAddress3() const { return csr.raddr3; }
    uint32_t csrWriteAddress() const { return csr.waddr; }
    uint32_t csrVectorLength() const { return csr.vlen; }

  private:
    struct CsrState
    {
        uint32_t status = 0;
        uint32_t raddr1 = 0;
        uint32_t raddr2 = 0;
        uint32_t raddr3 = 0;
        uint32_t waddr = 0;
        uint32_t config = 0;
        uint32_t vlen = 0;
        uint32_t mask = 0xffffffffu;
    };

    enum class ControlState : uint8_t { Idle, Respond, Recovery };

    struct SourceChunk
    {
        uint32_t chunk = 0;
        VeuVector data = {};
    };

    struct ResultToken
    {
        uint32_t chunk = 0;
        VeuFunctionalResult result;
        uint64_t readyCycle = 0;
    };

    struct Outstanding
    {
        TimingVeuMemoryRequest request;
        bool responseQueued = false;
    };

    struct PendingResponse
    {
        uint64_t transactionId = 0;
        bool isWrite = false;
        VeuVector data = {};
    };

    static constexpr uint32_t FullWriteMask = 0xffffffffu;

    uint32_t readCsr(uint16_t addr) const;
    void writeCsr(uint16_t addr, uint32_t value, VeuWriteType writeType);
    void acceptRequest(const VeuRequest &request);
    void startVectorOperation(const VeuRequest &request);
    VeuInstruction decodeStart(uint32_t veStart) const;
    void advanceOperation();
    void processResponses();
    void advancePipelines();
    void acceptVfuInput();
    void issueOneMemoryRequest();
    bool issueRequest(const TimingVeuMemoryRequest &request);
    std::optional<TimingVeuMemoryRequest> makeReadRequest();
    void completeChunk(uint32_t chunk);
    void enterDrainingIfDone();
    void completeOperation();
    bool allSourcesReady(uint32_t chunk) const;
    SourceChunk takeSourceChunk(VeuSource source, uint32_t chunk);
    uint32_t sourceAddress(VeuSource source, uint32_t chunk) const;
    std::string sourceSetName() const;
    std::string maskClassName() const;
    bool readCanIssue() const;
    void trace(const char *event, int32_t chunk = -1,
               VeuSource source = VeuSource::None,
               uint64_t transactionId = 0, Addr address = 0,
               const VeuVector *data = nullptr, uint32_t strobe = 0,
               const std::string &detail = "");

    VeuTimingConfig timing;
    VeuTimingProfile timingProfile;
    VeuTimingSelection activeTiming;
    MemoryRequestFn memoryRequest;
    State currentState = State::Idle;
    ControlState controlState = ControlState::Idle;
    CsrState csr;
    VeuRequest requestReg;
    uint32_t responseData = 0;
    bool pendingThreeSourceStart = false;
    uint32_t pendingThreeSourceVeStart = 0;

    bool statusBusy = false;
    bool lockActive = false;
    bool internalActive = false;
    uint64_t modelCycle = 0;
    uint64_t operationId = 0;
    uint64_t nextOperationId = 1;
    uint64_t nextTransactionId = 1;
    uint64_t operationStartCycle = 0;
    uint64_t nextVfuAcceptCycle = 0;
    uint64_t drainReadyCycle = 0;
    VeuInstruction operationInstruction = VeuInstruction::Unknown;
    VeuOperationInfo operationInfo;
    uint32_t operationChunkCount = 0;
    uint32_t nextExecuteChunk = 0;
    uint32_t completedChunkCount = 0;
    uint8_t readRoundRobin = 0;
    std::array<uint32_t, 3> nextReadChunk = {};
    std::array<uint32_t, 3> outstandingBySource = {};
    std::array<std::deque<SourceChunk>, 3> inputFifos;
    std::deque<ResultToken> vfuPipeline;
    std::deque<ResultToken> vsuPipeline;
    std::deque<ResultToken> storeQueue;
    std::unordered_map<uint64_t, Outstanding> outstanding;
    std::deque<PendingResponse> pendingResponses;
    std::optional<TimingVeuMemoryRequest> retryRequest;
    VeuFunctionalExecutor functionalExecutor;
    std::ofstream traceStream;

    uint64_t acceptedRequests = 0;
    uint64_t responses = 0;
    uint64_t startedOperations = 0;
    uint64_t completedOperations = 0;
    uint64_t processedChunks = 0;
    uint64_t busyCycles = 0;
    uint64_t loadWaitCycles = 0;
    uint64_t executeCycles = 0;
    uint64_t storeWaitCycles = 0;
    uint64_t memoryReads = 0;
    uint64_t memoryWrites = 0;
    uint64_t statusActiveCycles = 0;
    uint64_t lockActiveCycles = 0;
    uint64_t maxOutstandingReadsSeen = 0;
    std::array<uint64_t, 3> maxFifoOccupancy = {};
    uint64_t fifoFullStalls = 0;
    uint64_t fifoEmptyStalls = 0;
    uint64_t vfuAccepted = 0;
    uint64_t vfuCompleted = 0;
    uint64_t maxVfuInFlight = 0;
    uint64_t vfuIIStalls = 0;
    uint64_t vsuQueueStalls = 0;
    uint64_t storePriorityCycles = 0;
    uint64_t readsBlockedByStore = 0;
    uint64_t maskedWrites = 0;
    uint64_t zeroMaskSkippedWrites = 0;
    uint64_t retries = 0;
    uint64_t unexpectedResponses = 0;
    uint64_t profileHits = 0;
    uint64_t profileMisses = 0;
    uint64_t profileFallbacks = 0;
    uint64_t zeroLengthNoops = 0;
    uint64_t illegalOperations = 0;
};

} // namespace brs
} // namespace gem5

#endif
