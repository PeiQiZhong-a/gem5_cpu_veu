#ifndef __BRS_MEMORY_DUT_KUI_MEMORY_MODEL_HH__
#define __BRS_MEMORY_DUT_KUI_MEMORY_MODEL_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <unordered_map>

#include "brs/memory/dut_kui_data_crossbar.hh"
#include "brs/memory/sram_converter_32to256.hh"
#include "brs/sau/sau_endpoint.hh"
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
    SauMemoryResponse sau;
    // Registered observability from the accelerator side of crossbar_mi.
    // Master indices use DutKuiDataMaster (SAU=0, VEU=1).
    std::array<bool, 2> masterAccepted{};
    std::array<bool, 2> masterDropped{};
    std::array<bool, 4> bankRequest{};
    bool sameBankCollision = false;
};

// Cycle model of the memory-facing portion of dut_kui. Instruction fetch is
// an independent 128-bit path. RV data accesses pass through the RTL's
// 32-to-256 converter, while TimingVEU accesses the 256-bit SRAM path directly.
class DutKuiMemoryModel
{
  public:
    struct Config
    {
        // The RTL application SRAM path has one registered latency stage in
        // addition to the unified tick boundary: a request observed on edge
        // N is returned on edge N+3 in the canonical pre-NBA trace.
        uint32_t ibusResponseLatency = 2;
        // crossbar_mi pipelines both the selected master and the SRAM ack by
        // two registers. DBUS uses the same crossbar pipeline before the
        // 256-to-32 converter samples the return.
        uint32_t crossbarMasterResponseLatency = 2;
        // The RV DBUS path has one additional registered return edge beyond
        // the native 256-bit master path.  Together with the converter this
        // makes a request at N expose dbus_resp at N+7 in the RTL trace.
        uint32_t crossbarDbusResponseLatency = 3;
        uint32_t instBase = 0x00000000;
        uint32_t instSize = 0x00040000;
        uint32_t dataBase = 0x29120000;
        uint32_t bankSize = 0x00010000;
        uint32_t bankCount = 4;
        uint32_t realBankCount = 3;
        uint32_t maxVeuOutstanding = 4;
    };

    DutKuiMemoryModel();
    explicit DutKuiMemoryModel(Config config);

    void reset();
    bool acceptIbus(const DutKuiIbusRequest &request);
    bool acceptDbus(const DutKuiDbusRequest &request, bool veuLockActive);
    bool acceptVeu(const DutKuiVeuRequest &request);
    // Outputs visible during the current cycle.  They remain stable until
    // clockEdge() commits the next edge.
    const DutKuiMemoryOutputs &evaluate() const { return visibleOutputs; }
    void clockEdge(
        bool veuLockActive,
        const SauMemoryOutput &sau = {});
    // Compatibility helper for existing callers: commit one edge and return
    // the outputs made visible by that edge.
    DutKuiMemoryOutputs clock(
        bool veuLockActive,
        const SauMemoryOutput &sau = {});

    void writeByte(uint32_t address, uint8_t value);
    uint8_t readByte(uint32_t address) const;
    void writeWord(uint32_t address, uint32_t value);
    uint32_t readWord(uint32_t address) const;

    uint32_t veuOutstandingCount() const { return veuOutstanding; }
    bool dbusHasOutstanding() const { return dbusOutstanding; }
    DutKuiDataCrossbar::State crossbarState() const
    {
        return dataCrossbar.state();
    }
    SramConverter32To256::State converterState() const
    {
        return dbusConverter.state();
    }
    bool ibusAcceptedThisTick() const { return ibusAcceptedThisCycle; }
    bool dbusAcceptedThisTick() const { return dbusAcceptedThisCycle; }
    bool veuAcceptedThisTick() const { return veuAcceptedThisCycle; }
    const DutKuiIbusRequest &currentIbusRequest() const
    {
        return acceptedIbus;
    }
    const DutKuiDbusRequest &currentDbusRequest() const
    {
        return acceptedDbus;
    }
    const DutKuiVeuRequest &currentVeuRequest() const
    {
        return acceptedVeu;
    }

  private:
    template <class Response>
    struct DelayedResponse
    {
        uint32_t remainingCycles = 0;
        Response response;
    };

    Config config;
    std::unordered_map<uint32_t, uint8_t> instructionMemory;
    SramConverter32To256 dbusConverter;
    DutKuiDataCrossbar dataCrossbar;

    bool ibusOutstanding = false;
    bool dbusOutstanding = false;
    uint32_t veuOutstanding = 0;
    bool veuAcceptedThisCycle = false;
    bool ibusAcceptedThisCycle = false;
    bool dbusAcceptedThisCycle = false;
    DutKuiIbusRequest acceptedIbus;
    DutKuiDbusRequest acceptedDbus;
    DutKuiVeuRequest acceptedVeu;
    std::deque<DutKuiVeuRequest> pendingVeuRequests;
    std::deque<DutKuiVeuRequest> issuedVeuRequests;
    bool previousVeuLockActive = false;

    std::deque<DelayedResponse<DutKuiIbusResponse>> ibusResponses;
    DutKuiMemoryOutputs visibleOutputs;
    bool instructionMapped(uint32_t address) const;
    bool dataMapped(uint32_t address) const;
    DutKuiMemoryOutputs advance(
        bool veuLockActive, const SauMemoryOutput &sau);
};

} // namespace brs
} // namespace gem5

#endif // __BRS_MEMORY_DUT_KUI_MEMORY_MODEL_HH__
