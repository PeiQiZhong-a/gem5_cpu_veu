#ifndef __BRS_FETCH_SOURCE_HH__
#define __BRS_FETCH_SOURCE_HH__

#include <cstdint>

namespace gem5
{

class FetchSource
{
  public:
    virtual ~FetchSource() = default;
    virtual bool canFetch(uint32_t addr) = 0;
    virtual uint32_t fetch32(uint32_t addr) = 0;
};

} // namespace gem5

#endif
