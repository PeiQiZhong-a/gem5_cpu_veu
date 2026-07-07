#ifndef __BRS_MEMORY_BACKEND_HH__
#define __BRS_MEMORY_BACKEND_HH__

#include <cstdint>

namespace gem5
{

class MemoryBackend
{
  public:
    virtual ~MemoryBackend() = default;
    virtual uint32_t load32(uint32_t addr) = 0;
    virtual uint8_t  load8(uint32_t addr) = 0;
    virtual uint16_t load16(uint32_t addr) = 0;

    virtual void store32(uint32_t addr, uint32_t data) = 0;
    virtual void store8(uint32_t addr, uint8_t data) = 0;
    virtual void store16(uint32_t addr, uint16_t data) = 0;
};

} // namespace gem5

#endif
