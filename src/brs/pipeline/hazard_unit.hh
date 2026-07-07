#ifndef __BRS_HAZARD_UNIT_HH__
#define __BRS_HAZARD_UNIT_HH__

#include <cstdint>
#include "brs/pipeline/pipeline_regs.hh"

namespace gem5
{

struct HazardDecision
{
    bool stall_pc = false;
    bool stall_ifid = false;
    bool bubble_idex = false;
};

class HazardUnit
{
  public:
    HazardUnit() = default;

    HazardDecision resolve(const IFID &ifid_cur,
                           const IDEX &idex_cur) const;
};

} // namespace gem5

#endif