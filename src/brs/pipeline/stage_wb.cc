#include "brs/pipeline/pipeline_core.hh"

namespace gem5
{

void
PipelineCore::stageWB()
{
    if (!memwb_cur.valid) {
        regs[0] = 0;
        return;
    }

    bool has_rd_write = false;
    uint32_t wb_data = 0;

    if (memwb_cur.reg_write && (memwb_cur.rd_fp || memwb_cur.rd != 0)) {
        switch (memwb_cur.wb_sel) {
          case WbSel::ALU:
            wb_data = memwb_cur.alu_result;
            if (memwb_cur.rd_fp) {
                fp_regs[memwb_cur.rd] = wb_data;
            } else {
                regs[memwb_cur.rd] = wb_data;
            }
            has_rd_write = true;
            break;
          case WbSel::MEM:
            wb_data = memwb_cur.mem_data;
            if (memwb_cur.rd_fp) {
                fp_regs[memwb_cur.rd] = wb_data;
            } else {
                regs[memwb_cur.rd] = wb_data;
            }
            has_rd_write = true;
            break;
          case WbSel::VEU:
            wb_data = memwb_cur.alu_result;
            regs[memwb_cur.rd] = wb_data;
            has_rd_write = true;
            break;
          default:
            break;
        }
    }

    if (memwb_cur.kind != InstrKind::NOP) {
        last_retire_event.valid = true;
        last_retire_event.pc = memwb_cur.pc;
        last_retire_event.instr = memwb_cur.instr;
        last_retire_event.regWrite = has_rd_write;
        last_retire_event.fpWrite = has_rd_write && memwb_cur.rd_fp;
        last_retire_event.rd = has_rd_write ? memwb_cur.rd : 0;
        last_retire_event.data = has_rd_write ? wb_data : 0;
        if (has_rd_write) {
            traceRetire(memwb_cur.pc, memwb_cur.instr,
                        true, memwb_cur.rd, wb_data);
        } else {
            traceRetire(memwb_cur.pc, memwb_cur.instr,
                        false, 0, 0);
        }

        ++retired_inst_count;
    }

    regs[0] = 0;
}

} // namespace gem5
