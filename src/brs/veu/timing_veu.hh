#ifndef __BRS_VEU_TIMING_VEU_HH__
#define __BRS_VEU_TIMING_VEU_HH__

#include <array>
#include <cstdint>
#include <functional>

#include "brs/veu/veu_endpoint.hh"

namespace gem5
{
namespace brs
{

struct VeuTimingConfig
{
    uint32_t inputFifoDepth = 4;
    uint32_t executeLatency = VeuComputeDelayCycles;
    uint32_t startupCycles = 0;
    uint32_t finishCycles = 0;
};

struct TimingVeuMemoryRequest
{
    uint32_t addr = 0;
    bool isWrite = false;
    std::array<uint8_t, VeuVectorBytes> data = {};
};

class TimingVeu : public VeuEndpoint
{
  public:
    enum class State : uint8_t
    {
        Idle,
        Startup,
        IssueLoad1,
        WaitLoad1,
        IssueLoad2,
        WaitLoad2,
        IssueLoad3,
        WaitLoad3,
        Execute,
        IssueStoreRead,
        WaitStoreRead,
        IssueStore,
        WaitStore,
        Finish
    };

    using MemoryRequestFn =
        std::function<bool(const TimingVeuMemoryRequest &)>;

    TimingVeu();

    void reset() override;
    VeuResponse evaluate() const override;
    void clock(const VeuRequest &request) override;

    void configure(const VeuTimingConfig &config);
    void setMemoryRequestCallback(MemoryRequestFn callback);
    void completeMemoryRead(uint32_t addr,
                            const std::array<uint8_t, VeuVectorBytes> &data);
    void completeMemoryWrite(uint32_t addr);

    State state() const { return currentState; }
    bool operationBusy() const { return currentState != State::Idle; }
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

    enum class PendingMemory : uint8_t
    {
        None,
        Load1,
        Load2,
        Load3,
        StoreRead,
        StoreWrite
    };

    enum class ControlState : uint8_t
    {
        Idle,
        Respond,
        Recovery
    };

    static constexpr uint32_t FullWriteMask = 0xffffffffu;

    uint32_t readCsr(uint16_t addr) const;
    void writeCsr(uint16_t addr, uint32_t value, VeuWriteType writeType);
    void acceptRequest(const VeuRequest &request);
    void startVectorOperation(const VeuRequest &request);
    bool issueMemory(PendingMemory pending, uint32_t addr, bool isWrite,
                     const std::array<uint8_t, VeuVectorBytes> &data = {});
    void computeCurrentChunk();
    void advanceAfterStore();
    void completeOperation();
    void advanceOperation();
    bool needsThirdOperand() const;
    bool needsSecondOperand() const;
    bool storesResult() const;
    bool writeMaskIsFull() const;
    VeuInstruction currentInstruction() const;

    static uint32_t loadLane(const std::array<uint8_t, VeuVectorBytes> &data,
                             uint32_t lane);
    static void storeLane(std::array<uint8_t, VeuVectorBytes> &data,
                          uint32_t lane, uint32_t value);
    static int32_t asSigned(uint32_t value);

    VeuTimingConfig timing;
    MemoryRequestFn memoryRequest;
    State currentState = State::Idle;
    ControlState controlState = ControlState::Idle;
    CsrState csr;
    VeuRequest requestReg;
    uint32_t responseData = 0;
    uint32_t remainingCycles = 0;
    uint32_t chunksRemaining = 0;
    uint32_t chunkIndex = 0;
    uint32_t currentReadAddr1 = 0;
    uint32_t currentReadAddr2 = 0;
    uint32_t currentReadAddr3 = 0;
    uint32_t currentWriteAddr = 0;
    uint32_t currentWriteMask = FullWriteMask;
    PendingMemory pendingMemory = PendingMemory::None;
    bool memoryResponseReady = false;
    bool pendingThreeSourceStart = false;
    uint32_t pendingThreeSourceVeStart = 0;

    std::array<uint8_t, VeuVectorBytes> input1 = {};
    std::array<uint8_t, VeuVectorBytes> input2 = {};
    std::array<uint8_t, VeuVectorBytes> input3 = {};
    std::array<uint8_t, VeuVectorBytes> result = {};
    std::array<uint8_t, VeuVectorBytes> storeMerge = {};

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
};

} // namespace brs
} // namespace gem5

#endif // __BRS_VEU_TIMING_VEU_HH__
