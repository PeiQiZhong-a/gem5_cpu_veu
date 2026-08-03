#ifndef __SAU_N_IM2COL_ADDRESS_HH__
#define __SAU_N_IM2COL_ADDRESS_HH__

#include <cstdint>

#include "sau_n/im2col_types.hh"

namespace gem5::sau_n
{

struct ChwAddress
{
    uint64_t wordOffset = 0;
    uint16_t row = 0;
    uint8_t bank = 0;
    uint8_t laneSel = 0;

    bool operator==(const ChwAddress &other) const
    {
        return wordOffset == other.wordOffset && row == other.row &&
               bank == other.bank && laneSel == other.laneSel;
    }
};

class ChwAddressMapper
{
  public:
    explicit ChwAddressMapper(const ResolvedConfig &config);

    ChwAddress locate(
        uint64_t n, uint64_t c, uint64_t h, uint64_t w) const;

    const ResolvedConfig &config() const { return resolved; }
    const DerivedConfig &derived() const { return dimensions; }

  private:
    ResolvedConfig resolved;
    DerivedConfig dimensions;
};

uint8_t tbActValueV1(
    uint64_t n, uint64_t c, uint64_t h, uint64_t w);

} // namespace gem5::sau_n

#endif // __SAU_N_IM2COL_ADDRESS_HH__
