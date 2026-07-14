#ifndef __BRS_MEMORY_TB_CROSSBAR_MODEL_HH__
#define __BRS_MEMORY_TB_CROSSBAR_MODEL_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <unordered_map>

namespace gem5
{
namespace brs
{

struct TbBusRequest
{
    bool valid = false;
    uint32_t address = 0;
    bool read = false;
    bool write = false;
    uint16_t writeStrobe = 0;
    std::array<uint32_t, 4> writeData{};
};

struct TbBusResponse
{
    bool valid = false;
    std::array<uint32_t, 4> readData{};
};

enum class TbBusMaster : uint8_t
{
    None,
    IBus,
    DBus,
    Veu
};

struct TbCrossbarInputs
{
    TbBusRequest ibus;
    TbBusRequest dbus;
    TbBusRequest veu;
    bool lockStart = false;
    bool lockFinish = false;
};

struct TbCrossbarOutputs
{
    TbBusResponse ibus;
    TbBusResponse dbus;
    TbBusResponse veu;
    TbBusMaster grantedMaster = TbBusMaster::None;
    bool lockActive = false;
};

// Cycle model of aerith/sim/src/ip/crossbar.sv plus its synchronous SRAMs.
// It intentionally models the testbench contract, not a generic gem5 memory.
class TbCrossbarModel
{
  public:
    struct Config
    {
        uint32_t ibusResponseDelay = 2;
        uint32_t dbusResponseDelay = 2;
        uint32_t veuPipelineStages = 3;
        uint32_t instBase = 0x00000000;
        uint32_t instSize = 0x00040000;
        uint32_t dataBase = 0x20010000;
        uint32_t dataSize = 0x00040000;
        // aerith_tb_top.sv instantiates data_sram with DEPTH=32768 32-bit
        // words: 128 KiB of physical storage behind the 256 KiB decode.
        uint32_t dataStorageSize = 0x00020000;
        uint32_t uartBase = 0x40000000;
        uint32_t uartSize = 0x00001000;
    };

    TbCrossbarModel();
    explicit TbCrossbarModel(Config config);

    void reset();
    TbCrossbarOutputs clock(const TbCrossbarInputs &inputs);

    void writeByte(uint32_t address, uint8_t value);
    uint8_t readByte(uint32_t address) const;
    void writeWord(uint32_t address, uint32_t value);
    uint32_t readWord(uint32_t address) const;

  private:
    struct PendingRequest
    {
        bool valid = false;
        TbBusRequest request;
    };

    struct DelayedResponse
    {
        uint32_t remainingCycles = 0;
        TbBusMaster master = TbBusMaster::None;
        TbBusResponse response;
    };

    Config config;
    bool lockActive = false;
    bool normalBusy = false;
    PendingRequest ibusPending;
    PendingRequest dbusPending;
    std::deque<DelayedResponse> delayedResponses;
    std::unordered_map<uint32_t, uint8_t> memory;
    std::array<uint32_t, 4> veuReadDataPins{};

    bool mapped(uint32_t address) const;
    bool backedBySram(uint32_t address) const;
    bool mappedToUart(uint32_t address) const;
    TbBusMaster arbitrate(const TbCrossbarInputs &inputs) const;
    TbBusRequest selectedRequest(
        TbBusMaster master, const TbCrossbarInputs &inputs) const;
    TbBusResponse access(const TbBusRequest &request);
    uint32_t responseDelay(TbBusMaster master) const;
    void deliverResponses(TbCrossbarOutputs &outputs);
};

} // namespace brs
} // namespace gem5

#endif // __BRS_MEMORY_TB_CROSSBAR_MODEL_HH__
