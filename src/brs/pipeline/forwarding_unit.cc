#include "brs/pipeline/forwarding_unit.hh"
#include "brs/pipeline/source_operands.hh"

namespace gem5
{

ForwardDecision
ForwardingUnit::resolve(const IDEX &idex_cur,
                        const EXMEM &exmem_cur,
                        const MEMWB &memwb_cur) const
{
    ForwardDecision d;

    if (!idex_cur.valid) {
        return d;
    }

    const SourceOperands sources = decodeSourceOperands(idex_cur.instr);
    const auto selectSource = [&](bool used, uint8_t source)
    {
        if (!used || source == 0) {
            return ForwardSel::NONE;
        }

        // EX/MEM contains a load address rather than the loaded value.
        if (exmem_cur.valid &&
            exmem_cur.reg_write &&
            exmem_cur.wb_sel != WbSel::MEM &&
            exmem_cur.rd != 0 &&
            exmem_cur.rd == source) {
            return ForwardSel::FROM_EXMEM;
        }

        if (memwb_cur.valid &&
            memwb_cur.reg_write &&
            memwb_cur.rd != 0 &&
            memwb_cur.rd == source) {
            return ForwardSel::FROM_MEMWB;
        }

        return ForwardSel::NONE;
    };

    // A 路：rs1  x0不前递
    d.sel_a = selectSource(sources.usesRs1, sources.rs1);

    // B 路：rs2
    d.sel_b = selectSource(sources.usesRs2, sources.rs2);
    d.sel_c = selectSource(sources.usesRs3, sources.rs3);

    return d;
}

} // namespace gem5
