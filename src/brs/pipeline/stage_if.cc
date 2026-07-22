#include "brs/pipeline/pipeline_core.hh"

namespace gem5
{

void
PipelineCore::stageIF()
{
    if (csr_debug_mode) {
        ifid_next = {};
        if (!stall_ifid && !stall_pc && debug_instr_valid &&
            debug_instr_ready) {
            ifid_next.valid = true;
            ifid_next.pc = csr_dpc;
            ifid_next.instr = debug_instr_input;
            ifid_next.instr_len =
                (debug_instr_input & 0x3u) == 0x3u ? 4 : 2;
            ifid_next.debug_instr = true;
        }
        return;
    }

    if (halt_requested) {
        ifid_next = {};
        return;
    }

    const uint32_t fetchLimit = text_end_termination ?
        text_end : 0xffffffffu;

    if (requestTimingFetch) {
        if (redirect_pc) {
            ifid_next = {};
        }

        FrontendFetchUnit::Input in;
        in.stall = stall_ifid || stall_pc;
        in.redirect = redirect_pc;
        in.redirectTarget = redirect_target;
        in.textEnd = fetchLimit;
        in.responseValid = fetch_block_response_valid;
        in.response = fetch_block_response;

        fetch_block_response_valid = false;
        fetch_block_response = {};

        const auto out = frontend.step(in);

        if (redirect_pc) {
            ifid_next = {};
        } else if (stall_ifid || stall_pc) {
            ifid_next = ifid_cur;
        } else {
            ifid_next = {};
        }

        if (!redirect_pc && !stall_ifid && !stall_pc &&
            out.instValid && out.pc < fetchLimit) {
            ifid_next.valid = true;
            ifid_next.pc = out.pc;
            ifid_next.instr = out.instr;
            ifid_next.instr_len = out.instrLen;
        }

        if (out.requestValid) {
            if (requestTimingFetch(out.requestFetchAddr)) {
                frontend.markRequestIssued();
                fetch_waiting = true;
            }
        }

        pc = frontend.getPC();
        return;
    }

    if (redirect_pc) {
        ifid_next = {};
        pc = redirect_target;
        return;
    }

    if (stall_ifid || stall_pc) {
        ifid_next = ifid_cur;
        return;
    }

    ifid_next = {};

    if (fetchInstr) {
        uint32_t inst = 0;
        if (pc < fetchLimit && fetchInstr(pc, inst)) {
            ifid_next.valid = true;
            ifid_next.pc = pc;
            ifid_next.instr = inst;
            ifid_next.instr_len = 4;
            pc += 4;
        }
        return;
    }

    if (((pc - text_start) >> 2) < program.program_words) {
        ifid_next.valid = true;
        ifid_next.pc = pc;
        ifid_next.instr = program.instr_mem[(pc - text_start) >> 2];
        ifid_next.instr_len = 4;
        pc += 4;
    }
}

} // namespace gem5
