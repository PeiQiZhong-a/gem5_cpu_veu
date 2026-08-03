#ifndef __BRS_MEMORY_SRAM_128_PROTOCOL_HH__
#define __BRS_MEMORY_SRAM_128_PROTOCOL_HH__

#include <array>
#include <cstdint>

namespace gem5
{
namespace brs
{

constexpr uint32_t Sram128Bits = 128;
constexpr uint32_t Sram128Bytes = Sram128Bits / 8;

// Byte-addressed SRAM port at the frozen npu_lpnpu SAU boundary.
//
// valid is a per-cycle beat strobe, not a ready/valid level handshake. Every
// cycle with valid=true is a distinct request. The addressed 16-byte line is
// address & ~0x0f. writeStrobe bit N controls writeData byte N at line+N; a
// zero strobe denotes a read.
struct Sram128Request
{
    bool valid = false;
    uint32_t address = 0;
    uint16_t writeStrobe = 0;
    std::array<uint8_t, Sram128Bytes> writeData{};

    constexpr bool isWrite() const { return writeStrobe != 0; }
};

struct Sram128Response
{
    // One valid pulse is returned for every accepted request, including
    // writes. Read data is meaningful only in that valid cycle.
    bool valid = false;
    std::array<uint8_t, Sram128Bytes> readData{};
};

} // namespace brs
} // namespace gem5

#endif // __BRS_MEMORY_SRAM_128_PROTOCOL_HH__
