#ifndef __BRS_MEMORY_SRAM_32_PROTOCOL_HH__
#define __BRS_MEMORY_SRAM_32_PROTOCOL_HH__

#include <cstdint>

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

} // namespace brs
} // namespace gem5

#endif // __BRS_MEMORY_SRAM_32_PROTOCOL_HH__
