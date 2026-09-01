#ifndef __BRS_MEMORY_NPU_LPNPU_MIKUI_MEMORY_MODEL_HH__
#define __BRS_MEMORY_NPU_LPNPU_MIKUI_MEMORY_MODEL_HH__

#include <cstdint>
#include <deque>
#include <unordered_map>

#include "brs/memory/dut_kui_memory_model.hh"
#include "brs/memory/npu_lpnpu_mikui_crossbar.hh"
#include "brs/memory/npu_lpnpu_mikui_dma_crossbar.hh"
#include "brs/memory/sram_converter_32to128.hh"

namespace gem5
{
namespace brs
{

// Memory-facing cycle model of npu_lpnpu hardware/src/dut_mikui.sv. Public
// request/response structs are shared with the legacy testbench model so the
// CPU wrapper can select either platform without changing PipelineCore.
class NpuLpnpuMikuiMemoryModel
{
  public:
    struct Config
    {
        uint32_t instBase = 0x00000000;
        uint32_t instSize = 0x00004000;
        uint32_t maxVeuOutstanding = 4;
        bool dmaTopology = false;
    };

    NpuLpnpuMikuiMemoryModel();
    explicit NpuLpnpuMikuiMemoryModel(Config config);

    void reset();
    bool acceptIbus(const DutKuiIbusRequest &request);
    bool acceptDbus(const DutKuiDbusRequest &request, bool veuLockActive);
    bool acceptVeu(const DutKuiVeuRequest &request);
    const DutKuiMemoryOutputs &evaluate() const { return visibleOutputs; }
    void clockEdge(bool veuLockActive, const SauMemoryOutput &sau = {});
    DutKuiMemoryOutputs clock(
        bool veuLockActive, const SauMemoryOutput &sau = {});

    void writeByte(uint32_t address, uint8_t value);
    uint8_t readByte(uint32_t address) const;
    void writeWord(uint32_t address, uint32_t value);
    uint32_t readWord(uint32_t address) const;

    uint32_t veuOutstandingCount() const { return veuOutstanding; }
    bool dbusHasOutstanding() const { return dbusOutstanding; }
    NpuLpnpuMikuiCrossbar::State crossbarState() const
    {
        return crossbar.state();
    }
    SramConverter32To128::State converterState() const
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
    Config config;
    std::unordered_map<uint32_t, uint8_t> instructionMemory;
    SramConverter32To128 dbusConverter;
    NpuLpnpuMikuiCrossbar crossbar;
    NpuLpnpuMikuiDmaCrossbar dmaCrossbar;
    DutKuiMemoryOutputs visibleOutputs;

    bool ibusOutstanding = false;
    bool dbusOutstanding = false;
    uint32_t veuOutstanding = 0;
    bool ibusAcceptedThisCycle = false;
    bool dbusAcceptedThisCycle = false;
    bool veuAcceptedThisCycle = false;
    DutKuiIbusRequest acceptedIbus;
    DutKuiDbusRequest acceptedDbus;
    DutKuiVeuRequest acceptedVeu;

    std::deque<DutKuiVeuRequest> pendingVeuRequests;
    std::deque<DutKuiVeuRequest> issuedVeuRequests;
    bool previousVeuLockActive = false;
    uint8_t dmaDbusWordOffset = 0;
    bool dmaDbusWrite = false;

    bool instructionMapped(uint32_t address) const;
    Sram128Request currentVeuBeat() const;
    DutKuiMemoryOutputs advance(
        bool veuLockActive, const SauMemoryOutput &sau);
};

} // namespace brs
} // namespace gem5

#endif // __BRS_MEMORY_NPU_LPNPU_MIKUI_MEMORY_MODEL_HH__
