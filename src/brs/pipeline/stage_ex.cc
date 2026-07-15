#include "brs/pipeline/pipeline_core.hh"
#include "brs/pipeline/veu_issue.hh"

#include <cstdint>
#include <iostream>
namespace gem5
{

namespace
{

constexpr uint32_t Int32Min = 0x80000000u;
constexpr uint32_t MinusOne = 0xffffffffu;
constexpr uint32_t MultiplierLatencyCycles = 3;

bool
isMduInstr(InstrKind kind)
{
    return kind == InstrKind::MUL || kind == InstrKind::MULH ||
           kind == InstrKind::MULHSU || kind == InstrKind::MULHU ||
           kind == InstrKind::DIV || kind == InstrKind::DIVU ||
           kind == InstrKind::REM || kind == InstrKind::REMU;
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

    if (idex_cur.kind == InstrKind::VEU) {
        if (veuCbuOutput.complete) {
            exmem_next.valid = true;
            exmem_next.pc = idex_cur.pc;
            exmem_next.kind = idex_cur.kind;
            exmem_next.rd = idex_cur.rd;
            exmem_next.alu_result = veuCbuOutput.result;
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
            if (veuCbuOutput.ready) {
                veuIssue = makeVeuCbuIssue(
                    idex_cur, op_a, op_b, op_c);
                if (veuIssue.valid) {
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

    exmem_next.valid = true;
    exmem_next.pc = idex_cur.pc;
    exmem_next.kind = idex_cur.kind;
    exmem_next.rd = idex_cur.rd;
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
          exmem_next.alu_result = static_cast<uint32_t>(
              static_cast<int32_t>(op_a) + idex_cur.imm);
          break;

      case InstrKind::SB:
      case InstrKind::SH:
      case InstrKind::SW:
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
          bool taken = false;

          switch (idex_cur.kind) {
            case InstrKind::BEQ:  taken = (op_a == op_b); break;
            case InstrKind::BNE:  taken = (op_a != op_b); break;
            case InstrKind::BLT:  taken = (static_cast<int32_t>(op_a) <  static_cast<int32_t>(op_b)); break;
            case InstrKind::BGE:  taken = (static_cast<int32_t>(op_a) >= static_cast<int32_t>(op_b)); break;
            case InstrKind::BLTU: taken = (op_a < op_b); break;
            case InstrKind::BGEU: taken = (op_a >= op_b); break;
            default: break;
          }

          if (taken) {
              redirect_pc = true;
              redirect_target = static_cast<uint32_t>(
                  static_cast<int32_t>(idex_cur.pc) + idex_cur.imm);
              flush_idex = true;
              ++flush_count;
          }

          exmem_next.alu_result = 0;
          break;
      }

      case InstrKind::JALR:
          exmem_next.alu_result = idex_cur.pc + idex_cur.instr_len;
          redirect_pc = true;
          redirect_target = static_cast<uint32_t>(
              (static_cast<int32_t>(op_a) + idex_cur.imm)) & ~1u;
          flush_idex = true;
          ++flush_count;
          break;

      case InstrKind::JAL:
        exmem_next.alu_result = idex_cur.pc + idex_cur.instr_len;

        redirect_pc = true;
        redirect_target = static_cast<uint32_t>(
            static_cast<int32_t>(idex_cur.pc) + idex_cur.imm);
        flush_idex = true;
        ++flush_count;
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
          exmem_next.alu_result = 0;
          break;

      case InstrKind::ECALL:
      case InstrKind::EBREAK:
          halt_requested = true;
          flush_idex = true;   
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
