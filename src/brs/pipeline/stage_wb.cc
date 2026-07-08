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

    if (memwb_cur.reg_write && memwb_cur.rd != 0) {
        switch (memwb_cur.wb_sel) {
          case WbSel::ALU:
            wb_data = memwb_cur.alu_result;
            regs[memwb_cur.rd] = wb_data;
            has_rd_write = true;
            break;
          case WbSel::MEM:
            wb_data = memwb_cur.mem_data;
            regs[memwb_cur.rd] = wb_data;
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
