#include "brs/pipeline/pipeline_core.hh"

namespace gem5
{

static bool
isStoreInstr(InstrKind kind)
{
    return kind == InstrKind::SB || kind == InstrKind::SH ||
           kind == InstrKind::SW || kind == InstrKind::FSW;
}

void
PipelineCore::stageMEM()
{
    memwb_next = {};


    if (!exmem_cur.valid) {
        return;
    }

    const bool isStore = isStoreInstr(exmem_cur.kind);

    memwb_next.valid = true;
    memwb_next.pc = exmem_cur.pc;
    memwb_next.kind = exmem_cur.kind;
    memwb_next.rd = exmem_cur.rd;
    memwb_next.rd_fp = exmem_cur.rd_fp;
    memwb_next.reg_write = exmem_cur.reg_write;
    memwb_next.wb_sel = exmem_cur.wb_sel;
    memwb_next.alu_result = exmem_cur.alu_result;
    memwb_next.mem_data = exmem_cur.mem_data;
    memwb_next.instr = exmem_cur.instr;
    memwb_next.instr_len = exmem_cur.instr_len;

    if (isStore) {
        const uint32_t store_val = (exmem_cur.kind == InstrKind::SB) ?
            (exmem_cur.store_data & 0xff) :
            ((exmem_cur.kind == InstrKind::SH) ?
                (exmem_cur.store_data & 0xffff) : exmem_cur.store_data);
        traceStore(exmem_cur.pc, exmem_cur.instr,
                   exmem_cur.alu_result, store_val);
    }

}

} // namespace gem5
