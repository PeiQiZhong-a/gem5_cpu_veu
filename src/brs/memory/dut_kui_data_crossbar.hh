#ifndef __BRS_MEMORY_DUT_KUI_DATA_CROSSBAR_HH__
#define __BRS_MEMORY_DUT_KUI_DATA_CROSSBAR_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <unordered_map>

#include "brs/memory/sram_256_protocol.hh"

namespace gem5
{
namespace brs
{

enum class DutKuiDataMaster : uint8_t
{
    Sau = 0,
    Veu = 1
};

struct DutKuiDataCrossbarInputs
{
    Sram256Request dbus;
    std::array<Sram256Request, 2> masters{};
    std::array<bool, 2> crossbarStart{};
    std::array<bool, 2> crossbarDone{};
};

struct DutKuiDataCrossbarOutputs
{
    Sram256Response dbus;
    std::array<Sram256Response, 2> masters{};
    std::array<bool, 2> acceptedMaster{};
    // A same-bank collision is not retried by the RTL crossbar.  The earlier
    // master in the RTL loop is overwritten and receives no response.
    std::array<bool, 2> droppedMaster{};
    bool sameBankCollision = false;
    bool acceptedDbus = false;
    std::array<bool, 4> bankRequest{};
};

// Cycle model of dut_kui's crossbar_mi plus the four wrapper-facing ports.
// Ports 0..2 are backed by real SRAM; port 3 is the wrapper's dummy port.
class DutKuiDataCrossbar
{
  public:
    enum class State : uint8_t
    {
        Idle,
        Active,
        RvActive
    };

    struct Config
    {
        uint32_t dataBase = 0x29120000;
        uint32_t bankSize = 0x00010000;
        uint32_t portCount = 4;
        uint32_t realBankCount = 3;
        uint32_t masterResponseLatency = 2;
        uint32_t dbusResponseLatency = 2;
    };

    DutKuiDataCrossbar();
    explicit DutKuiDataCrossbar(Config config);

    void reset();
    void clearMemory();
    DutKuiDataCrossbarOutputs evaluate() const;
    void clock(const DutKuiDataCrossbarInputs &inputs);

    State state() const { return currentState; }
    int decodeBank(uint32_t address) const;
    bool isRealBank(uint32_t bank) const;
    void writeByte(uint32_t address, uint8_t value);
    uint8_t readByte(uint32_t address) const;
    void writeWord(uint32_t address, uint32_t value);
    uint32_t readWord(uint32_t address) const;

  private:
    struct PendingResponse
    {
        uint32_t remainingCycles = 0;
        bool dbus = false;
        uint8_t master = 0;
        Sram256Response response;
    };

    Config config;
    State currentState = State::Idle;
    DutKuiDataCrossbarOutputs outputReg;
    bool dbusLevelIssued = false;
    std::deque<PendingResponse> pendingResponses;
    std::unordered_map<uint32_t, uint8_t> memory;

    Sram256Response access(uint32_t bank, const Sram256Request &request);
    void advanceResponses();
};

} // namespace brs
} // namespace gem5

#endif // __BRS_MEMORY_DUT_KUI_DATA_CROSSBAR_HH__
