#include "brs/pipeline/hazard_unit.hh"

namespace gem5
{

HazardDecision
HazardUnit::resolve(const IFID &, const IDEX &) const
{
    // Spirit holds IF/ID while the load remains in IEU. Once the LSU
    // completes, its value is immediately available on the IE bypass and
    // the dependent instruction may advance without an extra bubble.
    return {};
}

} // namespace gem5
