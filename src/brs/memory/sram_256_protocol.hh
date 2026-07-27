#ifndef __BRS_MEMORY_SRAM_256_PROTOCOL_HH__
#define __BRS_MEMORY_SRAM_256_PROTOCOL_HH__

#include <array>
#include <cstdint>

namespace gem5
{
namespace brs
{

constexpr uint32_t Sram256Bits = 256;
constexpr uint32_t Sram256Bytes = Sram256Bits / 8;

// Byte-addressed 256-bit SRAM port used at the SAU/system boundary.
//
// valid is a per-cycle beat strobe, not a ready/valid level handshake.  Every
// cycle with valid=true is a distinct request, including consecutive cycles.
// The producer must not hold one request waiting for a response.  The P4 RTL
// has no request-ready or retry signal.
//
// The addressed 32-byte line is address & ~0x1f.  writeStrobe bit N controls
// writeData byte N at line address + N; a zero strobe denotes a read.
struct Sram256Request
{
    bool valid = false;
    uint32_t address = 0;
    uint32_t writeStrobe = 0;
    std::array<uint8_t, Sram256Bytes> writeData{};

    constexpr bool isWrite() const { return writeStrobe != 0; }
};

struct Sram256Response
{
    // One valid pulse is returned for every request accepted by the crossbar,
    // including writes. Read data is meaningful only in that valid cycle.
    // SA_CORE consumes it according to its configured fixed SRAM delay.
    bool valid = false;
    std::array<uint8_t, Sram256Bytes> readData{};
};

} // namespace brs
} // namespace gem5

#endif // __BRS_MEMORY_SRAM_256_PROTOCOL_HH__
