#include "brs/pipeline/hazard_unit.hh"
#include "brs/pipeline/source_operands.hh"

namespace gem5
{

HazardDecision
HazardUnit::resolve(const IFID &ifid_cur, const IDEX &idex_cur) const
{
    HazardDecision hz;

    if (!ifid_cur.valid || !idex_cur.valid) {
        return hz;
    }

    if (!idex_cur.mem_read || idex_cur.rd == 0) {
        return hz;
    }

    const SourceOperands sources = decodeSourceOperands(ifid_cur.instr);
    const bool hazardRs1 =
        sources.usesRs1 && sources.rs1 == idex_cur.rd;
    const bool hazardRs2 =
        sources.usesRs2 && sources.rs2 == idex_cur.rd;
    const bool hazardRs3 =
        sources.usesRs3 && sources.rs3 == idex_cur.rd;

    if (hazardRs1 || hazardRs2 || hazardRs3) {
        hz.stall_pc = true;
        hz.stall_ifid = true;
        hz.bubble_idex = true;
    }

    return hz;
}

} // namespace gem5
