#ifndef __BRS_MEMORY_SRAM_CONVERTER_32TO256_HH__
#define __BRS_MEMORY_SRAM_CONVERTER_32TO256_HH__

#include <cstdint>

#include "brs/memory/sram_256_protocol.hh"

namespace gem5
{
namespace brs
{

struct Sram32Request
{
    bool valid = false;
    uint32_t address = 0;
    uint8_t writeStrobe = 0;
    uint32_t writeData = 0;

    constexpr bool isWrite() const { return writeStrobe != 0; }
};

struct Sram32Response
{
    bool valid = false;
    uint32_t readData = 0;
    bool isWrite = false;
};

struct SramConverter32To256Output
{
    Sram256Request sram;
    Sram32Response master;
};

// Cycle model of hardware/src/crossbar/sram_converter_32to256.sv.
class SramConverter32To256
{
  public:
    enum class State : uint8_t
    {
        Idle,
        WaitAck
    };

    void reset();
    SramConverter32To256Output evaluate() const;
    void clock(
        const Sram32Request &masterRequest,
        const Sram256Response &sramResponse);

    State state() const { return currentState; }
    bool canAccept() const { return currentState == State::Idle; }

  private:
    State currentState = State::Idle;
    Sram256Request requestReg;
    uint8_t wordOffsetReg = 0;
    bool writeReg = false;
    Sram32Response responseReg;
};

} // namespace brs
} // namespace gem5

#endif // __BRS_MEMORY_SRAM_CONVERTER_32TO256_HH__
