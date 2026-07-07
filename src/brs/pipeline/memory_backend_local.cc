#include "brs/pipeline/memory_backend_local.hh"

#include <cassert>
#include <cstring>

namespace gem5
{

LocalMemoryBackend::LocalMemoryBackend()
{
    reset();
}

uint32_t
LocalMemoryBackend::toOffset(uint32_t addr) const
{
    assert(addr >= kDataBase);
    const uint32_t off = addr - kDataBase;
    assert(off < kDataBytes);
    return off;
}

void
LocalMemoryBackend::reset()
{
    std::memset(data_mem, 0, sizeof(data_mem));

    // 先保留一个最小 bring-up 默认值
    // 地址 0x80000000 -> 42
    data_mem[0] = 42;
    data_mem[1] = 0;
    data_mem[2] = 0;
    data_mem[3] = 0;

    // 地址 0x80000004 -> 100
    data_mem[4] = 100;
    data_mem[5] = 0;
    data_mem[6] = 0;
    data_mem[7] = 0;

    // 地址 0x80000008 -> 7
    data_mem[8]  = 7;
    data_mem[9]  = 0;
    data_mem[10] = 0;
    data_mem[11] = 0;


    data_mem[0x1000] = 0x42;
    data_mem[0x1001] = 0x00;
}

uint8_t
LocalMemoryBackend::load8(uint32_t addr)
{
    const uint32_t off = toOffset(addr);
    return data_mem[off];
}

uint16_t
LocalMemoryBackend::load16(uint32_t addr)
{
    assert((addr & 0x1) == 0);
    const uint32_t off = toOffset(addr);
    assert(off + 1 < kDataBytes);

    return static_cast<uint16_t>(data_mem[off]) |
           (static_cast<uint16_t>(data_mem[off + 1]) << 8);
}

uint32_t
LocalMemoryBackend::load32(uint32_t addr)
{
    assert((addr & 0x3) == 0);
    const uint32_t off = toOffset(addr);
    assert(off + 3 < kDataBytes);

    return static_cast<uint32_t>(data_mem[off]) |
           (static_cast<uint32_t>(data_mem[off + 1]) << 8) |
           (static_cast<uint32_t>(data_mem[off + 2]) << 16) |
           (static_cast<uint32_t>(data_mem[off + 3]) << 24);
}

void
LocalMemoryBackend::store8(uint32_t addr, uint8_t data)
{
    const uint32_t off = toOffset(addr);
    data_mem[off] = data;
}

void
LocalMemoryBackend::store16(uint32_t addr, uint16_t data)
{
    assert((addr & 0x1) == 0);
    const uint32_t off = toOffset(addr);
    assert(off + 1 < kDataBytes);

    data_mem[off]     = static_cast<uint8_t>(data & 0xFF);
    data_mem[off + 1] = static_cast<uint8_t>((data >> 8) & 0xFF);
}

void
LocalMemoryBackend::store32(uint32_t addr, uint32_t data)
{
    assert((addr & 0x3) == 0);
    const uint32_t off = toOffset(addr);
    assert(off + 3 < kDataBytes);

    data_mem[off]     = static_cast<uint8_t>(data & 0xFF);
    data_mem[off + 1] = static_cast<uint8_t>((data >> 8) & 0xFF);
    data_mem[off + 2] = static_cast<uint8_t>((data >> 16) & 0xFF);
    data_mem[off + 3] = static_cast<uint8_t>((data >> 24) & 0xFF);
}

} // namespace gem5