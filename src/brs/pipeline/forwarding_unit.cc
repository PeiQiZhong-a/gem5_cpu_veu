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
    const bool fpInstruction =
        idex_cur.kind == InstrKind::FLW || idex_cur.kind == InstrKind::FSW ||
        idex_cur.kind == InstrKind::FMV_X_W ||
        idex_cur.kind == InstrKind::FMV_W_X ||
        idex_cur.kind == InstrKind::FP_ARITH;
    const auto selectSource = [&](bool used, uint8_t source, bool sourceFp)
    {
        if (!used || (!sourceFp && source == 0)) {
            return ForwardSel::NONE;
        }

        // Loads only enter EX/MEM after the IEU-style LSU transaction has
        // completed, so alu_result contains the loaded value and can be
        // forwarded exactly like the RTL IEU bypass.
        if (exmem_cur.valid &&
            exmem_cur.reg_write &&
            exmem_cur.rd_fp == sourceFp &&
            (sourceFp || exmem_cur.rd != 0) &&
            exmem_cur.rd == source) {
            return ForwardSel::FROM_EXMEM;
        }

        if (memwb_cur.valid &&
            memwb_cur.reg_write &&
            memwb_cur.rd_fp == sourceFp &&
            (sourceFp || memwb_cur.rd != 0) &&
            memwb_cur.rd == source) {
            return ForwardSel::FROM_MEMWB;
        }

        return ForwardSel::NONE;
    };

    // A 路：rs1  x0不前递
    const uint32_t opcode = idex_cur.instr & 0x7f;
    const uint32_t funct7 = (idex_cur.instr >> 25) & 0x7f;
    const bool fusedFp = idex_cur.kind == InstrKind::FP_ARITH &&
        (opcode == 0x43 || opcode == 0x47 || opcode == 0x4b ||
         opcode == 0x4f);
    const bool binaryFp = idex_cur.kind == InstrKind::FP_ARITH &&
        opcode == 0x53 &&
        (funct7 == 0x00 || funct7 == 0x04 || funct7 == 0x08 ||
         funct7 == 0x0c || funct7 == 0x10 || funct7 == 0x14 ||
         funct7 == 0x50);
    const bool usesRs1 = fpInstruction ? true : sources.usesRs1;
    const bool usesRs2 = fpInstruction ?
        (idex_cur.kind == InstrKind::FSW || fusedFp || binaryFp) :
        sources.usesRs2;
    const bool usesRs3 = fusedFp;

    d.sel_a = selectSource(usesRs1, idex_cur.rs1, idex_cur.rs1_fp);

    // B 路：rs2
    d.sel_b = selectSource(usesRs2, idex_cur.rs2, idex_cur.rs2_fp);
    d.sel_c = selectSource(usesRs3, idex_cur.rs3, idex_cur.rs3_fp);

    return d;
}

} // namespace gem5
