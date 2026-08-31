#ifndef __BRS_MEMORY_NPU_LPNPU_MIKUI_DMA_CROSSBAR_HH__
#define __BRS_MEMORY_NPU_LPNPU_MIKUI_DMA_CROSSBAR_HH__

#include <array>
#include <cstdint>
#include <unordered_map>

#include "brs/memory/npu_lpnpu_mikui_crossbar.hh"

namespace gem5
{
namespace brs
{

struct NpuLpnpuMikuiDmaCrossbarOutputs
{
    Sram128Response dbus;
    std::array<Sram128Response, 2> masters{};
    std::array<bool, 2> acceptedMaster{};
    std::array<bool, 2> droppedMaster{};
    std::array<bool, 3> bankRequest{};
    bool sameBankCollision = false;
    bool addressError = false;
};

// Register-edge model of crossbar_mi_full as instantiated by
// hardware/src/top/dut_mikui_dma.sv.  The default A/B/C/D split inputs used
// by top_mikui_dma_tb create one independent ISRAM plus three 32-KiB data
// SRAM ports (stack, ping and pong).
class NpuLpnpuMikuiDmaCrossbar
{
  public:
    enum class State : uint8_t
    {
        Idle = 0,
        Active = 1
    };

    struct Config
    {
        uint32_t splitBase = 0x20008000;
        uint32_t splitEnd = 0x20028000;
        uint8_t sramB = 4;
        uint8_t sramC = 8;
        uint8_t sramD = 12;
    };

    NpuLpnpuMikuiDmaCrossbar();
    explicit NpuLpnpuMikuiDmaCrossbar(Config config);

    void reset();
    NpuLpnpuMikuiDmaCrossbarOutputs evaluate() const
    {
        return outputReg;
    }
    void clock(const NpuLpnpuMikuiCrossbarInputs &inputs);

    State state() const { return currentState; }
    int decodeBank(uint32_t address) const;
    uint32_t bankStart(uint8_t bank) const;
    uint32_t bankEnd(uint8_t bank) const;
    void writeByte(uint32_t address, uint8_t value);
    uint8_t readByte(uint32_t address) const;

  private:
    Config config;
    State currentState = State::Idle;
    std::array<bool, 2> masterRunning{};
    bool transactionOverLatched = false;
    bool dbusPending = false;
    Sram128Request pendingDbus;
    bool dbusInflight = false;
    std::array<std::unordered_map<uint32_t, uint8_t>, 3> memory;
    NpuLpnpuMikuiDmaCrossbarOutputs outputReg;

    std::array<uint8_t, Sram128Bytes> readLine(
        uint8_t bank, uint32_t address) const;
    void accessBank(uint8_t bank, const Sram128Request &request);
};

} // namespace brs
} // namespace gem5

#endif // __BRS_MEMORY_NPU_LPNPU_MIKUI_DMA_CROSSBAR_HH__
