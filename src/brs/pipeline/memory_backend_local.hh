#ifndef __BRS_MEMORY_BACKEND_LOCAL_HH__
#define __BRS_MEMORY_BACKEND_LOCAL_HH__

#include <cstdint>
#include "brs/pipeline/memory_backend.hh"

namespace gem5
{

class LocalMemoryBackend : public MemoryBackend
{
  public:
    static constexpr uint32_t kDataBytes = 0x4000;
    static constexpr uint32_t kDataBase  = 0x80000000u;
     

    LocalMemoryBackend();

    uint8_t  load8(uint32_t addr) override;
    uint16_t load16(uint32_t addr) override;
    uint32_t load32(uint32_t addr) override;

    void store32(uint32_t addr, uint32_t data) override;
    void store8(uint32_t addr, uint8_t data) override;
    void store16(uint32_t addr, uint16_t data) override;

    void reset();

  private:
    uint32_t toOffset(uint32_t addr) const;
    uint8_t data_mem[kDataBytes];
};

} // namespace gem5

#endif