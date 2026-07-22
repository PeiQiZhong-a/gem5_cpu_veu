#ifndef __BRS_MEMORY_DUT_KUI_MEMORY_MODEL_HH__
#define __BRS_MEMORY_DUT_KUI_MEMORY_MODEL_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <unordered_map>

#include "brs/veu/veu_functional.hh"

namespace gem5
{
namespace brs
{

struct DutKuiIbusRequest
{
    uint32_t address = 0;
};

struct DutKuiDbusRequest
{
    uint32_t address = 0;
    uint8_t writeStrobe = 0;
    uint32_t writeData = 0;

    bool isWrite() const { return writeStrobe != 0; }
};

struct DutKuiVeuRequest
{
    uint64_t transactionId = 0;
    uint32_t address = 0;
    bool isWrite = false;
    uint32_t writeStrobe = 0;
    VeuVector data = {};
};

struct DutKuiIbusResponse
{
    bool valid = false;
    std::array<uint32_t, 4> readData{};
};

struct DutKuiDbusResponse
{
    bool valid = false;
    bool isWrite = false;
    uint32_t readData = 0;
};

struct DutKuiVeuResponse
{
    bool valid = false;
    uint64_t transactionId = 0;
    bool isWrite = false;
    VeuVector readData = {};
};

struct DutKuiMemoryOutputs
{
    DutKuiIbusResponse ibus;
    DutKuiDbusResponse dbus;
    DutKuiVeuResponse veuRead;
    DutKuiVeuResponse veuWrite;
};

// Cycle model of the memory-facing portion of dut_kui. Instruction fetch is
// an independent 128-bit path. RV data accesses pass through the RTL's
// 32-to-256 converter, while TimingVEU accesses the 256-bit SRAM path directly.
class DutKuiMemoryModel
{
  public:
    struct Config
    {
        uint32_t ibusResponseLatency = 3;
        uint32_t dbusResponseLatency = 4;
        // PipelineMiniCPU delivers the model response after the current core
        // step; TimingVEU consumes it on the following edge. These internal
        // delays therefore produce RTL-visible issue-to-return deltas of 4/1.
        uint32_t veuReadLatency = 3;
        uint32_t veuWriteLatency = 0;
        uint32_t instBase = 0x00000000;
        uint32_t instSize = 0x00040000;
        uint32_t dataBase = 0x29120000;
        uint32_t bankSize = 0x00010000;
        uint32_t bankCount = 4;
        uint32_t maxVeuOutstanding = 4;
    };

    DutKuiMemoryModel();
    explicit DutKuiMemoryModel(Config config);

    void reset();
    bool acceptIbus(const DutKuiIbusRequest &request);
    bool acceptDbus(const DutKuiDbusRequest &request, bool veuLockActive);
    bool acceptVeu(const DutKuiVeuRequest &request);
    DutKuiMemoryOutputs clock(bool veuLockActive);

    void writeByte(uint32_t address, uint8_t value);
    uint8_t readByte(uint32_t address) const;
    void writeWord(uint32_t address, uint32_t value);
    uint32_t readWord(uint32_t address) const;

    uint32_t veuOutstandingCount() const { return veuOutstanding; }
    bool dbusHasOutstanding() const { return dbusOutstanding; }

  private:
    template <class Response>
    struct DelayedResponse
    {
        uint32_t remainingCycles = 0;
        Response response;
    };

    Config config;
    std::unordered_map<uint32_t, uint8_t> instructionMemory;
    std::unordered_map<uint32_t, uint8_t> dataMemory;

    bool ibusOutstanding = false;
    bool dbusOutstanding = false;
    uint32_t veuOutstanding = 0;
    bool veuAcceptedThisCycle = false;
    bool ibusAcceptedThisCycle = false;
    bool dbusAcceptedThisCycle = false;
    DutKuiIbusRequest acceptedIbus;
    DutKuiDbusRequest acceptedDbus;
    DutKuiVeuRequest acceptedVeu;

    std::deque<DelayedResponse<DutKuiIbusResponse>> ibusResponses;
    std::deque<DelayedResponse<DutKuiDbusResponse>> dbusResponses;
    std::deque<DelayedResponse<DutKuiVeuResponse>> veuReadResponses;
    std::deque<DelayedResponse<DutKuiVeuResponse>> veuWriteResponses;

    bool instructionMapped(uint32_t address) const;
    bool dataMapped(uint32_t address) const;
    uint32_t alignedDataAddress(uint32_t address) const;
    VeuVector readVector(uint32_t address) const;
    void writeVector(uint32_t address, uint32_t strobe,
                     const VeuVector &data);
};

} // namespace brs
} // namespace gem5

#endif // __BRS_MEMORY_DUT_KUI_MEMORY_MODEL_HH__
