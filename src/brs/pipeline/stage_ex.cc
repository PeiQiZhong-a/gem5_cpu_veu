#include "brs/pipeline/pipeline_core.hh"
#include "brs/pipeline/sau_issue.hh"
#include "brs/pipeline/veu_issue.hh"

#include <cstdint>
#include <iostream>

#include "base/logging.hh"
namespace gem5
{

namespace
{

constexpr uint32_t Int32Min = 0x80000000u;
constexpr uint32_t MinusOne = 0xffffffffu;
// Multiplier.sv accepts in state 0, stalls through states 0 and 1, and
// exposes ready in state 2. The instruction therefore occupies three EX
// evaluations with two stalled evaluations before completion.
constexpr uint32_t MultiplierLatencyCycles = 2;

bool
isMduInstr(InstrKind kind)
{
    return kind == InstrKind::MUL || kind == InstrKind::MULH ||
           kind == InstrKind::MULHSU || kind == InstrKind::MULHU ||
           kind == InstrKind::DIV || kind == InstrKind::DIVU ||
           kind == InstrKind::REM || kind == InstrKind::REMU;
}

bool
isLoadInstr(InstrKind kind)
{
    return kind == InstrKind::LB || kind == InstrKind::LBU ||
           kind == InstrKind::LH || kind == InstrKind::LHU ||
           kind == InstrKind::LW || kind == InstrKind::FLW;
}

bool
isStoreInstr(InstrKind kind)
{
    return kind == InstrKind::SB || kind == InstrKind::SH ||
           kind == InstrKind::SW || kind == InstrKind::FSW;
}

unsigned
memAccessSize(InstrKind kind)
{
    switch (kind) {
      case InstrKind::LB:
      case InstrKind::LBU:
      case InstrKind::SB:
        return 1;
      case InstrKind::LH:
      case InstrKind::LHU:
      case InstrKind::SH:
        return 2;
      case InstrKind::LW:
      case InstrKind::SW:
      case InstrKind::FLW:
      case InstrKind::FSW:
        return 4;
      default:
        return 0;
    }
}

uint32_t
signOrZeroExtendLoad(InstrKind kind, uint32_t value, uint32_t address)
{
    const unsigned byteOffset = address & 0x3u;
    const uint32_t selectedByte = (value >> (byteOffset * 8)) & 0xffu;
    // LSU.sv uses addr[1:0] == 0 for the low halfword and the high
    // halfword for all other offsets.  It never assembles across words.
    const uint32_t selectedHalf = byteOffset == 0 ?
        (value & 0xffffu) : ((value >> 16) & 0xffffu);

    switch (kind) {
      case InstrKind::LB:
        return static_cast<uint32_t>(
            static_cast<int32_t>(static_cast<int8_t>(selectedByte)));
      case InstrKind::LBU:
        return selectedByte;
      case InstrKind::LH:
        return static_cast<uint32_t>(
            static_cast<int32_t>(static_cast<int16_t>(selectedHalf)));
      case InstrKind::LHU:
        return selectedHalf;
      case InstrKind::LW:
      case InstrKind::FLW:
      default:
        return value;
    }
}

uint32_t
clz32(uint32_t value)
{
    if (value == 0) {
        return 32;
    }

    uint32_t count = 0;
    for (uint32_t bit = 0x80000000u; (value & bit) == 0; bit >>= 1) {
        ++count;
    }
    return count;
}

uint32_t
absOperand(uint32_t value, bool isSigned)
{
    return isSigned && (value & Int32Min) ? static_cast<uint32_t>(-value) :
        value;
}

__int128
mduOperand(uint32_t value, bool isSigned)
{
    return isSigned ? static_cast<__int128>(
                          static_cast<int64_t>(static_cast<int32_t>(value))) :
                      static_cast<__int128>(static_cast<uint64_t>(value));
}

uint64_t
mduMultiplyResult(uint32_t lhs, uint32_t rhs, bool lhsSigned, bool rhsSigned)
{
    const auto product =
        mduOperand(lhs, lhsSigned) * mduOperand(rhs, rhsSigned);
    return static_cast<uint64_t>(static_cast<unsigned __int128>(product));
}

uint64_t
mduDivideResult(uint32_t lhs, uint32_t rhs, bool lhsSigned, bool rhsSigned)
{
    uint32_t quotient = 0;
    uint32_t remainder = 0;

    if (rhs == 0) {
        quotient = MinusOne;
        remainder = lhs;
    } else if (rhs == 1) {
        quotient = lhs;
        remainder = 0;
    } else if (lhsSigned && rhsSigned && rhs == MinusOne) {
        quotient = 0u - lhs;
        remainder = 0;
    } else if (lhsSigned || rhsSigned) {
        const int32_t lhsValue = static_cast<int32_t>(lhs);
        const int32_t rhsValue = rhsSigned ? static_cast<int32_t>(rhs) :
            static_cast<int32_t>(static_cast<uint32_t>(rhs));
        quotient = static_cast<uint32_t>(lhsValue / rhsValue);
        remainder = static_cast<uint32_t>(lhsValue % rhsValue);
    } else {
        quotient = lhs / rhs;
        remainder = lhs % rhs;
    }

    return (static_cast<uint64_t>(remainder) << 32) | quotient;
}

bool
mduDivideSkips(uint32_t lhs, uint32_t rhs, bool lhsSigned, bool rhsSigned)
{
    const uint32_t lhsAbs = absOperand(lhs, lhsSigned);
    const uint32_t rhsAbs = absOperand(rhs, rhsSigned);
    return rhs == 1 || (rhsSigned && rhs == MinusOne) ||
           rhs == 0 || lhsAbs < rhsAbs;
}

uint32_t
mduDivideLatency(uint32_t lhs, uint32_t rhs, bool lhsSigned, bool rhsSigned)
{
    if (mduDivideSkips(lhs, rhs, lhsSigned, rhsSigned)) {
        return 1;
    }

    const uint32_t lhsAbs = absOperand(lhs, lhsSigned);
    const uint32_t rhsAbs = absOperand(rhs, rhsSigned);
    const uint32_t shift = 31 - clz32(rhsAbs) + clz32(lhsAbs);
    const uint32_t cnt = 15 - (shift >> 1);
    return cnt + 3;
}

uint32_t
mduLatency(const IDEX &idex, uint32_t lhs, uint32_t rhs)
{
    if (!idex.mdu_op_is_div) {
        return MultiplierLatencyCycles;
    }

    return mduDivideLatency(lhs, rhs,
                            idex.mdu_op1_is_signed,
                            idex.mdu_op2_is_signed);
}

uint32_t
mduResult(const IDEX &idex, uint32_t lhs, uint32_t rhs)
{
    const uint64_t result = idex.mdu_op_is_div ?
        mduDivideResult(lhs, rhs,
                        idex.mdu_op1_is_signed,
                        idex.mdu_op2_is_signed) :
        mduMultiplyResult(lhs, rhs,
                          idex.mdu_op1_is_signed,
                          idex.mdu_op2_is_signed);
    return idex.mdu_output_is_high ?
        static_cast<uint32_t>(result >> 32) :
        static_cast<uint32_t>(result);
}

} // anonymous namespace

void
PipelineCore::stageEX()
{
    exmem_next = {};
    exmem_next.instr = idex_cur.instr;

    if (!idex_cur.valid) {
        return;
    }

    uint32_t op_a = idex_cur.rs1_val;
    uint32_t op_b = idex_cur.rs2_val;
    uint32_t op_c = idex_cur.rs3_val;

    const ForwardDecision fwd =
        forwardingUnit.resolve(idex_cur, exmem_cur, memwb_cur);

    uint32_t memwb_value = 0;
    switch (memwb_cur.wb_sel) {
      case WbSel::ALU:
        memwb_value = memwb_cur.alu_result;
        break;
      case WbSel::MEM:
        memwb_value = memwb_cur.mem_data;
        break;
      case WbSel::VEU:
      case WbSel::SAU:
        memwb_value = memwb_cur.alu_result;
        break;
      default:
        memwb_value = 0;
        break;
      
    }

    if (fwd.sel_a == ForwardSel::FROM_EXMEM) {
        op_a = exmem_cur.alu_result;
    } else if (fwd.sel_a == ForwardSel::FROM_MEMWB) {
        op_a = memwb_value;
    }

    if (fwd.sel_b == ForwardSel::FROM_EXMEM) {
        op_b = exmem_cur.alu_result;
    } else if (fwd.sel_b == ForwardSel::FROM_MEMWB) {
        op_b = memwb_value;
    }

    if (fwd.sel_c == ForwardSel::FROM_EXMEM) {
        op_c = exmem_cur.alu_result;
    } else if (fwd.sel_c == ForwardSel::FROM_MEMWB) {
        op_c = memwb_value;
    }

    if (fwd.sel_a != ForwardSel::NONE) {
        ++forward_count;
    }

    if (fwd.sel_b != ForwardSel::NONE) {
        ++forward_count;
    }

    if (fwd.sel_c != ForwardSel::NONE) {
        ++forward_count;
    }

    if (idex_cur.kind == InstrKind::SAU) {
        if (hcCbuOutput.complete) {
            exmem_next.valid = true;
            exmem_next.pc = idex_cur.pc;
            exmem_next.kind = idex_cur.kind;
            exmem_next.rd = idex_cur.rd;
            exmem_next.alu_result = hcCbuOutput.result;
            exmem_next.instr = idex_cur.instr;
            exmem_next.instr_len = idex_cur.instr_len;
            exmem_next.reg_write = idex_cur.reg_write;
            exmem_next.mem_read = false;
            exmem_next.mem_write = false;
            exmem_next.wb_sel = idex_cur.wb_sel;
            ++sau_complete_count;
        } else {
            exmem_next = {};
            ++sau_csr_handshake_cycles;
            if (hcCbuOutput.ready) {
                hcIssue = makeSauHcCbuIssue(idex_cur, op_a, op_b);
                if (hcIssue.valid) {
                    ++sau_issue_count;
                }
            }
            sau_stall = true;
        }
        return;
    }

    if (idex_cur.kind == InstrKind::VEU) {
        if (hcCbuOutput.complete) {
            exmem_next.valid = true;
            exmem_next.pc = idex_cur.pc;
            exmem_next.kind = idex_cur.kind;
            exmem_next.rd = idex_cur.rd;
            exmem_next.rd_fp = idex_cur.rd_fp;
            exmem_next.alu_result = hcCbuOutput.result;
            exmem_next.instr = idex_cur.instr;
            exmem_next.instr_len = idex_cur.instr_len;
            exmem_next.reg_write = idex_cur.reg_write;
            exmem_next.mem_read = false;
            exmem_next.mem_write = false;
            exmem_next.wb_sel = idex_cur.wb_sel;
            ++veu_complete_count;
        } else {
            exmem_next = {};
            ++veu_csr_handshake_cycles;
            if (hcCbuOutput.ready) {
                hcIssue = makeVeuHcCbuIssue(
                    idex_cur, op_a, op_b, op_c);
                if (hcIssue.valid) {
                    ++veu_issue_count;
                }
            }
            veu_stall = true;
        }
        return;
    }

    if (isMduInstr(idex_cur.kind)) {
        if (!mdu_busy) {
            mdu_busy = true;
            mdu_cycles_remaining = mduLatency(idex_cur, op_a, op_b);
            mdu_result = mduResult(idex_cur, op_a, op_b);
        }

        if (mdu_cycles_remaining > 0) {
            --mdu_cycles_remaining;
            mdu_stall = true;
            exmem_next = {};
            return;
        }
    }

    // The generated RTL has RV32F decode and an FP register file, but its
    // IEU has no connected FPU.  RES_RV32F therefore asserts stall_req
    // continuously.  Preserve that exact behavior until the RTL itself is
    // regenerated with a real FPU interface.
    if (idex_cur.kind == InstrKind::FP_ARITH) {
        fp_stall = true;
        exmem_next = {};
        return;
    }

    const bool isLoad = isLoadInstr(idex_cur.kind);
    const bool isStore = isStoreInstr(idex_cur.kind);
    if (isLoad || isStore) {
        if (timingVeuOwnsSharedDmem()) {
            lsu_stall = true;
            ++rv_dmem_blocked_by_veu_cycles;
            exmem_next = {};
            return;
        }

        const uint32_t computedAddress = static_cast<uint32_t>(
            static_cast<int32_t>(op_a) + idex_cur.imm);
        const unsigned size = memAccessSize(idex_cur.kind);

        panic_if(!requestTimingData,
                 "Timing data backend is not configured for memory access");

        if (!mem_req_issued && !data_waiting && !data_response_valid) {
            if (requestTimingData(computedAddress, size, isStore, op_b)) {
                mem_req_issued = true;
                mem_request_addr = computedAddress;
                mem_request_store_data = op_b;
                data_waiting = true;
            }
            lsu_stall = true;
            exmem_next = {};
            return;
        }

        if (!data_response_valid) {
            lsu_stall = true;
            exmem_next = {};
            return;
        }

        const uint32_t address = mem_req_issued ?
            mem_request_addr : computedAddress;
        const uint32_t storeData = mem_req_issued ?
            mem_request_store_data : op_b;

        panic_if(data_response_addr != address,
                 "Data response address mismatch: response=%#x expected=%#x",
                 data_response_addr, address);
        panic_if(data_response_is_store != isStore,
                 "Data response type mismatch for pc=%#x", idex_cur.pc);

        exmem_next.valid = true;
        exmem_next.pc = idex_cur.pc;
        exmem_next.kind = idex_cur.kind;
        exmem_next.rd = idex_cur.rd;
        exmem_next.rd_fp = idex_cur.rd_fp;
        exmem_next.store_data = storeData;
        exmem_next.instr = idex_cur.instr;
        exmem_next.instr_len = idex_cur.instr_len;
        exmem_next.reg_write = idex_cur.reg_write;
        exmem_next.mem_read = idex_cur.mem_read;
        exmem_next.mem_write = idex_cur.mem_write;
        exmem_next.wb_sel = idex_cur.wb_sel;

        if (isLoad) {
            const uint32_t loaded =
                signOrZeroExtendLoad(idex_cur.kind, data_response_value,
                                     address);
            exmem_next.alu_result = loaded;
            exmem_next.mem_data = loaded;
        } else {
            exmem_next.alu_result = address;
        }

        mem_req_issued = false;
        mem_request_addr = 0;
        mem_request_store_data = 0;
        data_waiting = false;
        data_response_valid = false;
        data_response_addr = 0;
        data_response_value = 0;
        data_response_is_store = false;
        return;
    }

    exmem_next.valid = true;
    exmem_next.pc = idex_cur.pc;
    exmem_next.kind = idex_cur.kind;
    exmem_next.rd = idex_cur.rd;
    exmem_next.rd_fp = idex_cur.rd_fp;
    exmem_next.reg_write = idex_cur.reg_write;
    exmem_next.mem_read = idex_cur.mem_read;
    exmem_next.mem_write = idex_cur.mem_write;
    exmem_next.wb_sel = idex_cur.wb_sel;
    exmem_next.instr_len = idex_cur.instr_len;

    exmem_next.store_data = op_b;

    switch (idex_cur.kind) {
      case InstrKind::ADDI:
          exmem_next.alu_result = static_cast<uint32_t>(
              static_cast<int32_t>(op_a) + idex_cur.imm);
          break;

      case InstrKind::ADD:
          exmem_next.alu_result = op_a + op_b;
          break;

      case InstrKind::SUB:
          exmem_next.alu_result = op_a - op_b;
          break;

      case InstrKind::MUL:
      case InstrKind::MULH:
      case InstrKind::MULHSU:
      case InstrKind::MULHU:
      case InstrKind::DIV:
      case InstrKind::DIVU:
      case InstrKind::REM:
      case InstrKind::REMU:
          exmem_next.alu_result = mdu_result;
          mdu_busy = false;
          mdu_result = 0;
          break;

      case InstrKind::LB:
      case InstrKind::LBU:
      case InstrKind::LH:
      case InstrKind::LHU:
      case InstrKind::LW:
      case InstrKind::FLW:
          exmem_next.alu_result = static_cast<uint32_t>(
              static_cast<int32_t>(op_a) + idex_cur.imm);
          break;

      case InstrKind::SB:
      case InstrKind::SH:
      case InstrKind::SW:
      case InstrKind::FSW:
          exmem_next.alu_result = static_cast<uint32_t>(
              static_cast<int32_t>(op_a) + idex_cur.imm);
          break;

      case InstrKind::BEQ:
      case InstrKind::BNE:
      case InstrKind::BLT:
      case InstrKind::BGE:
      case InstrKind::BLTU:
      case InstrKind::BGEU:
      {
          // JCU already resolved this instruction in ID.
          exmem_next.alu_result = 0;
          break;
      }

      case InstrKind::JALR:
          exmem_next.alu_result = idex_cur.pc + idex_cur.instr_len;
          break;

      case InstrKind::JAL:
        exmem_next.alu_result = idex_cur.pc + idex_cur.instr_len;

        break;

      case InstrKind::FMV_X_W:
      case InstrKind::FMV_W_X:
        exmem_next.alu_result = op_a;
        break;

      case InstrKind::LUI:
        exmem_next.alu_result = static_cast<uint32_t>(idex_cur.imm);
        break;

      case InstrKind::AUIPC:
        exmem_next.alu_result = idex_cur.pc + static_cast<uint32_t>(idex_cur.imm);
        break;

      case InstrKind::XORI:
        exmem_next.alu_result = op_a ^ static_cast<uint32_t>(idex_cur.imm);
        break;

      case InstrKind::ORI:
        exmem_next.alu_result = op_a | static_cast<uint32_t>(idex_cur.imm);
        break;

      case InstrKind::ANDI:
        exmem_next.alu_result = op_a & static_cast<uint32_t>(idex_cur.imm);
        break;

      case InstrKind::SLTI:
        exmem_next.alu_result =
            (static_cast<int32_t>(op_a) < idex_cur.imm) ? 1u : 0u;
        break;

      case InstrKind::SLTIU:
        exmem_next.alu_result =
            (op_a < static_cast<uint32_t>(idex_cur.imm)) ? 1u : 0u;
        break;

      case InstrKind::XOR:
        exmem_next.alu_result = op_a ^ op_b;
        break;

      case InstrKind::OR:
        exmem_next.alu_result = op_a | op_b;
        break;

      case InstrKind::AND:
        exmem_next.alu_result = op_a & op_b;
        break;

      case InstrKind::SLT:
        exmem_next.alu_result =
            (static_cast<int32_t>(op_a) < static_cast<int32_t>(op_b)) ? 1u : 0u;
        break;

      case InstrKind::SLTU:
        exmem_next.alu_result = (op_a < op_b) ? 1u : 0u;
        break;

      case InstrKind::SLLI:
        exmem_next.alu_result = op_a << (static_cast<uint32_t>(idex_cur.imm) & 0x1F);
        break;

      case InstrKind::SRLI:
        exmem_next.alu_result = op_a >> (static_cast<uint32_t>(idex_cur.imm) & 0x1F);
        break;

      case InstrKind::SRAI:
        exmem_next.alu_result = static_cast<uint32_t>(
            static_cast<int32_t>(op_a) >> (static_cast<uint32_t>(idex_cur.imm) & 0x1F));
        break;

      case InstrKind::SLL:
        exmem_next.alu_result = op_a << (op_b & 0x1F);
        break;

      case InstrKind::SRL:
        exmem_next.alu_result = op_a >> (op_b & 0x1F);
        break;

      case InstrKind::SRA:
        exmem_next.alu_result = static_cast<uint32_t>(
            static_cast<int32_t>(op_a) >> (op_b & 0x1F));
         break;
      case InstrKind::FENCE:
      case InstrKind::FENCE_I:
      case InstrKind::WFI:
      case InstrKind::ECALL:
          exmem_next.alu_result = 0;
          break;

      case InstrKind::CSRRW:
      case InstrKind::CSRRS:
      case InstrKind::CSRRC:
      case InstrKind::CSRRWI:
      case InstrKind::CSRRSI:
      case InstrKind::CSRRCI:
          exmem_next.alu_result = idex_cur.csr_rdata;
          break;

      case InstrKind::EBREAK:
          if (ebreak_termination) {
              // EBREAK is an ordinary retiring terminator for explicitly
              // configured workload runs.  The default path below preserves
              // Spirit's architectural trap/debug behavior.
              exmem_next.alu_result = 0;
              break;
          }
          // DCSR may turn EBREAK into a debug entry.  Otherwise Spirit uses
          // internal exception code 4 and records the resolved sequential PC.
          if (!csr_debug_mode &&
              (csr_machine_mode ? csr_dcsr_ebreakm : csr_dcsr_ebreaku)) {
              enterDebug(idex_cur.pc + idex_cur.instr_len, 1);
          } else if (!csr_debug_mode) {
              enterTrap(idex_cur.pc + idex_cur.instr_len, 4, false);
          } else {
              debug_instr_caught_ebreak = true;
          }
          flush_idex = true;
          ++flush_count;
          exmem_next.alu_result = 0;
          break;

      case InstrKind::MRET:
          returnFromTrap();
          flush_idex = true;
          ++flush_count;
          exmem_next.alu_result = 0;
          break;

      default:
        exmem_next.alu_result = 0;
        break;
    }



    
    //debug  看看前递
    std::cout << "[FWD] rs1=" << unsigned(idex_cur.rs1)
          << " rs2=" << unsigned(idex_cur.rs2)
          << " rs3=" << unsigned(idex_cur.rs3)
          << " sel_a=" << static_cast<int>(fwd.sel_a)
          << " sel_b=" << static_cast<int>(fwd.sel_b)
          << " sel_c=" << static_cast<int>(fwd.sel_c)
          << " op_a=" << op_a
          << " op_b=" << op_b
          << " op_c=" << op_c
          << std::endl;

    
}

} // namespace gem5
