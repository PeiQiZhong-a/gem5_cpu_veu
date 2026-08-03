#ifndef __BRS_MEMORY_NPU_LPNPU_MIKUI_CROSSBAR_HH__
#define __BRS_MEMORY_NPU_LPNPU_MIKUI_CROSSBAR_HH__

#include <array>
#include <cstdint>
#include <unordered_map>

#include "brs/memory/sram_128_protocol.hh"

namespace gem5
{
namespace brs
{

enum class NpuLpnpuMikuiMaster : uint8_t
{
    Sau = 0,
    Veu = 1
};

struct NpuLpnpuMikuiCrossbarInputs
{
    Sram128Request dbus;
    std::array<Sram128Request, 2> masters{};
    std::array<bool, 2> crossbarStart{};
    std::array<bool, 2> crossbarDone{};
};

struct NpuLpnpuMikuiCrossbarOutputs
{
    Sram128Response dbus;
    // valid marks the cycle in which the registered RTL master_rdata is
    // updated from a bank. It is gem5 observability; crossbar_mi.sv does not
    // expose its internal master_ack ports.
    std::array<Sram128Response, 2> masters{};
    std::array<bool, 2> acceptedMaster{};
    std::array<bool, 2> droppedMaster{};
    std::array<bool, 2> bankRequest{};
    bool sameBankCollision = false;
};

// Register-level model of npu_lpnpu mikui_v2.0 crossbar_mi.sv. This class
// intentionally reproduces RTL behavior such as sticky RVACTIVE and retained
// slave request registers. It is not a cleaned-up ready/valid crossbar.
class NpuLpnpuMikuiCrossbar
{
  public:
    enum class State : uint8_t
    {
        Idle = 0,
        Active = 1,
        RvActive = 2
    };

    static constexpr uint32_t Bank0Base = 0x20010000;
    static constexpr uint32_t Bank0End = 0x2001ffff;
    static constexpr uint32_t Bank1Base = 0x20020000;
    static constexpr uint32_t Bank1End = 0x2002ffff;

    void reset();
    NpuLpnpuMikuiCrossbarOutputs evaluate() const { return outputReg; }
    void clock(const NpuLpnpuMikuiCrossbarInputs &inputs);

    State state() const { return currentState; }
    int decodeMasterBank(uint32_t address) const;
    uint8_t decodeDbusBank(uint32_t address) const;
    void writeByte(uint32_t address, uint8_t value);
    uint8_t readByte(uint32_t address) const;

  private:
    State currentState = State::Idle;
    bool busBusyReg = false;
    std::array<Sram128Request, 2> slaveRequestRegs{};
    std::array<bool, 2> slaveAckRegs{};
    std::array<std::array<uint8_t, Sram128Bytes>, 2> slaveReadDataRegs{};
    std::array<uint8_t, 2> selectStage1{};
    std::array<uint8_t, 2> selectStage2{};
    std::array<bool, 2> selectValidStage1{};
    std::array<bool, 2> selectValidStage2{};
    std::array<std::array<uint8_t, Sram128Bytes>, 2> masterReadDataRegs{};
    Sram128Response dbusOutputReg;
    NpuLpnpuMikuiCrossbarOutputs outputReg;
    std::array<std::unordered_map<uint32_t, uint8_t>, 2> memory;

    std::array<uint8_t, Sram128Bytes> readLine(
        uint8_t bank, uint32_t address) const;
    void accessBank(uint8_t bank, const Sram128Request &request);
};

} // namespace brs
} // namespace gem5

#endif // __BRS_MEMORY_NPU_LPNPU_MIKUI_CROSSBAR_HH__
