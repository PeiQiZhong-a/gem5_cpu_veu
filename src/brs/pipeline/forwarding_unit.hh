#ifndef __BRS_FORWARDING_UNIT_HH__
#define __BRS_FORWARDING_UNIT_HH__

#include <cstdint>
#include "brs/pipeline/pipeline_regs.hh"

namespace gem5
{

enum class ForwardSel : uint8_t
{
    NONE = 0,
    FROM_EXMEM = 1,
    FROM_MEMWB = 2
};

struct ForwardDecision
{
    ForwardSel sel_a = ForwardSel::NONE;
    ForwardSel sel_b = ForwardSel::NONE;
    ForwardSel sel_c = ForwardSel::NONE;
};

class ForwardingUnit
{
  public:
    ForwardingUnit() = default;

    ForwardDecision resolve(
        const IDEX &idex_cur,
        const EXMEM &exmem_cur,
        const MEMWB &memwb_cur) const;
};

} // namespace gem5

#endif
