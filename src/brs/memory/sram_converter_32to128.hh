#ifndef __BRS_MEMORY_SRAM_CONVERTER_32TO128_HH__
#define __BRS_MEMORY_SRAM_CONVERTER_32TO128_HH__

#include <cstdint>

#include "brs/memory/sram_32_protocol.hh"
#include "brs/memory/sram_128_protocol.hh"

namespace gem5
{
namespace brs
{

struct SramConverter32To128Output
{
    Sram128Request sram;
    Sram32Response master;
};

// Register-for-register model of npu_lpnpu's
// hardware/src/crossbar/sram_converter_32to128.sv.
class SramConverter32To128
{
  public:
    enum class State : uint8_t
    {
        Idle = 0,
        Convert = 1,
        WaitAck = 2
    };

    void reset();
    SramConverter32To128Output evaluate() const;
    void clock(
        const Sram32Request &masterRequest,
        const Sram128Response &sramResponse);

    State state() const { return currentState; }
    bool canAccept() const { return currentState == State::Idle; }

  private:
    State currentState = State::Idle;
    uint32_t addressReg = 0;
    std::array<uint8_t, Sram128Bytes> writeDataReg{};
    uint16_t writeStrobeReg = 0;
    uint8_t wordOffsetReg = 0;
    bool writeReg = false;

    Sram128Request sramOutputReg;
    Sram32Response masterOutputReg;
};

} // namespace brs
} // namespace gem5

#endif // __BRS_MEMORY_SRAM_CONVERTER_32TO128_HH__
