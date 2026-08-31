#include "brs/pipeline/pipeline_core.hh"
#include "brs/pipeline/compressed_decode.hh"
#include "brs/pipeline/sau_decode.hh"
#include "brs/pipeline/veu_decode.hh"

namespace gem5
{

namespace
{

enum class RtlFpKind
{
    None,
    Load,
    Store,
    MoveToInt,
    MoveToFp,
    Arithmetic
};

RtlFpKind
decodeRtlFpKind(uint32_t instr)
{
    const uint32_t opcode = instr & 0x7f;
    const uint32_t funct3 = (instr >> 12) & 0x7;
    const uint32_t rs2 = (instr >> 20) & 0x1f;
    const uint32_t funct7 = (instr >> 25) & 0x7f;

    if (opcode == 0x07 && funct3 == 0x2) {
        return RtlFpKind::Load;
    }
    if (opcode == 0x27 && funct3 == 0x2) {
        return RtlFpKind::Store;
    }
    if ((opcode == 0x43 || opcode == 0x47 || opcode == 0x4b ||
         opcode == 0x4f) && ((instr >> 25) & 0x3) == 0) {
        return RtlFpKind::Arithmetic;
    }
    if (opcode != 0x53) {
        return RtlFpKind::None;
    }

    if (funct7 == 0x70 && rs2 == 0 && funct3 == 0) {
        return RtlFpKind::MoveToInt;
    }
    if (funct7 == 0x78 && rs2 == 0 && funct3 == 0) {
        return RtlFpKind::MoveToFp;
    }

    const bool binaryArithmetic =
        funct7 == 0x00 || funct7 == 0x04 || funct7 == 0x08 ||
        funct7 == 0x0c;
    const bool sqrt = funct7 == 0x2c && rs2 == 0;
    const bool signInject = funct7 == 0x10 && funct3 <= 2;
    const bool minMax = funct7 == 0x14 && funct3 <= 1;
    const bool convertToInt = funct7 == 0x60 && rs2 <= 1;
    const bool compare = funct7 == 0x50 && funct3 <= 2;
    const bool classify = funct7 == 0x70 && rs2 == 0 && funct3 == 1;
    const bool convertToFloat = funct7 == 0x68 && rs2 <= 1;

    return binaryArithmetic || sqrt || signInject || minMax ||
           convertToInt || compare || classify || convertToFloat ?
        RtlFpKind::Arithmetic : RtlFpKind::None;
}

} // anonymous namespace

void
PipelineCore::stageID()
{
    idex_next = {};

    if (flush_idex) {       //flush 要比 bubble 优先
        return;   
    }

    if (bubble_idex) {
        return;   
    }

    if (!ifid_cur.valid) {
        return;
    }

    // With no debug instruction supplied by the testbench, PFU does not
    // present normal program instructions while CSRU is in debug mode.
    if (csr_debug_mode && !ifid_cur.debug_instr) {
        return;
    }

    // CSR register writes take effect at the edge; interrupt qualification
    // for this edge therefore uses the pre-write CSRU state.
    const bool takeDebug = !csr_debug_mode &&
        (debug_halt_sampled || debug_halt_on_reset_latched ||
         debug_step_halt_requested);
    const bool takeInterrupt = !csr_debug_mode && !takeDebug &&
                               interruptActive();
    const uint8_t pendingInterruptCode =
        takeInterrupt ? interruptCode() : 0;

    uint32_t instr = ifid_cur.instr;
    uint8_t instr_len = ifid_cur.instr_len;

    if (instr_len == 2 || isCompressedInstr(instr)) {
        if (!expandCompressedInstr(instr, instr)) {
            return;
        }
        instr_len = 2;
    }

    const uint32_t opcode = instr & 0x7F;
    const uint32_t rd = (instr >> 7) & 0x1F;
    const uint32_t funct3 = (instr >> 12) & 0x7;
    const uint32_t rs1 = (instr >> 15) & 0x1F;
    const uint32_t rs2 = (instr >> 20) & 0x1F;
    const uint32_t funct7 = (instr >> 25) & 0x7F;
    const uint32_t imm12 = (instr >> 20) & 0xFFF;
    const uint32_t imm_s = ((instr >> 25) << 5) | ((instr >> 7) & 0x1F);
    const uint32_t imm_b =
    (((instr >> 31) & 0x1) << 12) |
    (((instr >> 7)  & 0x1) << 11) |
    (((instr >> 25) & 0x3F) << 5) |
    (((instr >> 8)  & 0xF) << 1);

    const uint32_t imm_j =
    (((instr >> 31) & 0x1) << 20) |
    (((instr >> 12) & 0xFF) << 12) |
    (((instr >> 20) & 0x1) << 11) |
    (((instr >> 21) & 0x3FF) << 1);

    const uint32_t imm_u = instr & 0xFFFFF000u;

    const brs::SauDecodeInfo sau = brs::decodeSpiritSauInstruction(instr);
    if (sau.valid) {
      idex_next.valid = true;
      idex_next.pc = ifid_cur.pc;
      idex_next.instr = instr;
      idex_next.instr_len = instr_len;
      idex_next.kind = InstrKind::SAU;

      idex_next.rd = sau.rd;
      idex_next.rs1 = sau.rs1;
      idex_next.rs2 = sau.rs2;
      idex_next.rs1_val = regs[sau.rs1];
      idex_next.rs2_val = regs[sau.rs2];

      idex_next.sau_operation = sau.operation;
      idex_next.sau_csr_addr = sau.csrAddr;
      idex_next.sau_csr_read = sau.csrRead;
      idex_next.sau_csr_write = sau.csrWrite;
      idex_next.sau_write_type = sau.writeType;

      idex_next.reg_write = sau.writesRd;
      idex_next.wb_sel = WbSel::SAU;
      return;
    }

    const brs::VeuDecodeInfo veu = brs::decodeSpiritVeuInstruction(instr);
    if (veu.valid) {
      idex_next.valid = true;
      idex_next.pc = ifid_cur.pc;
      idex_next.instr = instr;
      idex_next.instr_len = instr_len;
      idex_next.kind = InstrKind::VEU;

      idex_next.rd = veu.rd;
      idex_next.rs1 = veu.rs1;
      idex_next.rs2 = veu.rs2;
      idex_next.rs3 = veu.rs3;
      idex_next.rs1_val = veu.usesRs1 ? regs[veu.rs1] : 0;
      idex_next.rs2_val = veu.usesRs2 ? regs[veu.rs2] : 0;
      idex_next.rs3_val = veu.usesRs3 ? regs[veu.rs3] : 0;

      idex_next.veu_operation = veu.operation;
      idex_next.veu_csr_addr = veu.csrAddr;
      idex_next.veu_csr_read = veu.csrRead;
      idex_next.veu_csr_write = veu.csrWrite;
      idex_next.veu_write_type = veu.writeType;
      idex_next.veu_start = veu.veStart;

      idex_next.reg_write = veu.writesRd;
      idex_next.wb_sel = veu.writesRd ? WbSel::VEU : WbSel::NONE;
      return;
    }

    const RtlFpKind fpKind = decodeRtlFpKind(instr);
    if (fpKind != RtlFpKind::None) {
      idex_next.valid = true;
      idex_next.pc = ifid_cur.pc;
      idex_next.instr = instr;
      idex_next.rd = static_cast<uint8_t>(rd);
      idex_next.rs1 = static_cast<uint8_t>(rs1);
      idex_next.rs2 = static_cast<uint8_t>(rs2);
      idex_next.rs3 = static_cast<uint8_t>((instr >> 27) & 0x1f);

      if (fpKind == RtlFpKind::Load) {
          idex_next.kind = InstrKind::FLW;
          idex_next.rd_fp = true;
          idex_next.rs1_val = regs[rs1];
          idex_next.imm = signExtend12(imm12);
          idex_next.mem_read = true;
          idex_next.reg_write = true;
          idex_next.wb_sel = WbSel::MEM;
      } else if (fpKind == RtlFpKind::Store) {
          idex_next.kind = InstrKind::FSW;
          idex_next.rs2_fp = true;
          idex_next.rs1_val = regs[rs1];
          idex_next.rs2_val = fp_regs[rs2];
          idex_next.imm = signExtend12(imm_s);
          idex_next.mem_write = true;
      } else if (fpKind == RtlFpKind::MoveToInt) {
          idex_next.kind = InstrKind::FMV_X_W;
          idex_next.rs1_fp = true;
          idex_next.rs1_val = fp_regs[rs1];
          idex_next.reg_write = true;
          idex_next.wb_sel = WbSel::ALU;
      } else if (fpKind == RtlFpKind::MoveToFp) {
          idex_next.kind = InstrKind::FMV_W_X;
          idex_next.rd_fp = true;
          idex_next.rs1_val = regs[rs1];
          idex_next.reg_write = true;
          idex_next.wb_sel = WbSel::ALU;
      } else {
          const bool fused = opcode == 0x43 || opcode == 0x47 ||
                             opcode == 0x4b || opcode == 0x4f;
          const bool intToFloat = opcode == 0x53 && funct7 == 0x68;
          const bool writesInteger = opcode == 0x53 &&
              (funct7 == 0x50 || funct7 == 0x60 || funct7 == 0x70);

          idex_next.kind = InstrKind::FP_ARITH;
          idex_next.rd_fp = !writesInteger;
          idex_next.rs1_fp = !intToFloat;
          idex_next.rs2_fp = !intToFloat;
          idex_next.rs3_fp = fused;
          idex_next.rs1_val = idex_next.rs1_fp ? fp_regs[rs1] : regs[rs1];
          idex_next.rs2_val = idex_next.rs2_fp ? fp_regs[rs2] : regs[rs2];
          idex_next.rs3_val = fused ? fp_regs[idex_next.rs3] : 0;
          idex_next.reg_write = true;
          idex_next.wb_sel = WbSel::ALU;
      }
    }

  // addi
    else if (opcode == 0x13 && funct3 == 0x0) {
      idex_next.valid = true;
      idex_next.pc = ifid_cur.pc;
      idex_next.instr = instr;
      idex_next.kind = InstrKind::ADDI;
      idex_next.rd = static_cast<uint8_t>(rd);
      idex_next.rs1 = static_cast<uint8_t>(rs1);
      idex_next.rs1_val = regs[rs1];
      idex_next.imm = signExtend12(imm12);
      idex_next.alu_op = AluOp::ADD;
      idex_next.reg_write = true;
      idex_next.wb_sel = WbSel::ALU;
    }
//add
    else if (opcode == 0x33 && funct3 == 0x0 && funct7 == 0x00) {
      idex_next.valid = true;
      idex_next.pc = ifid_cur.pc;
      idex_next.instr = instr;
      idex_next.kind = InstrKind::ADD;
      idex_next.rd = static_cast<uint8_t>(rd);
      idex_next.rs1 = static_cast<uint8_t>(rs1);
      idex_next.rs2 = static_cast<uint8_t>(rs2);
      idex_next.rs1_val = regs[rs1];
      idex_next.rs2_val = regs[rs2];
      idex_next.alu_op = AluOp::ADD;
      idex_next.reg_write = true;
      idex_next.wb_sel = WbSel::ALU;
    }
  // rv32m
    else if (opcode == 0x33 && funct7 == 0x01) {
      idex_next.valid = true;
      idex_next.pc = ifid_cur.pc;
      idex_next.instr = instr;
      idex_next.rd = static_cast<uint8_t>(rd);
      idex_next.rs1 = static_cast<uint8_t>(rs1);
      idex_next.rs2 = static_cast<uint8_t>(rs2);
      idex_next.rs1_val = regs[rs1];
      idex_next.rs2_val = regs[rs2];
      idex_next.reg_write = true;
      idex_next.wb_sel = WbSel::ALU;

      switch (funct3) {
        case 0x0:
          idex_next.kind = InstrKind::MUL;
          idex_next.alu_op = AluOp::MUL;
          idex_next.mdu_op1_is_signed = true;
          idex_next.mdu_op2_is_signed = true;
          break;
        case 0x1:
          idex_next.kind = InstrKind::MULH;
          idex_next.alu_op = AluOp::MULH;
          idex_next.mdu_op1_is_signed = true;
          idex_next.mdu_op2_is_signed = true;
          idex_next.mdu_output_is_high = true;
          break;
        case 0x2:
          idex_next.kind = InstrKind::MULHSU;
          idex_next.alu_op = AluOp::MULHSU;
          idex_next.mdu_op1_is_signed = true;
          idex_next.mdu_output_is_high = true;
          break;
        case 0x3:
          idex_next.kind = InstrKind::MULHU;
          idex_next.alu_op = AluOp::MULHU;
          idex_next.mdu_output_is_high = true;
          break;
        case 0x4:
          idex_next.kind = InstrKind::DIV;
          idex_next.alu_op = AluOp::DIV;
          idex_next.mdu_op_is_div = true;
          idex_next.mdu_op1_is_signed = true;
          idex_next.mdu_op2_is_signed = true;
          break;
        case 0x5:
          idex_next.kind = InstrKind::DIVU;
          idex_next.alu_op = AluOp::DIVU;
          idex_next.mdu_op_is_div = true;
          break;
        case 0x6:
          idex_next.kind = InstrKind::REM;
          idex_next.alu_op = AluOp::REM;
          idex_next.mdu_op_is_div = true;
          idex_next.mdu_op1_is_signed = true;
          idex_next.mdu_op2_is_signed = true;
          idex_next.mdu_output_is_high = true;
          break;
        case 0x7:
          idex_next.kind = InstrKind::REMU;
          idex_next.alu_op = AluOp::REMU;
          idex_next.mdu_op_is_div = true;
          idex_next.mdu_output_is_high = true;
          break;
        default:
          idex_next = {};
          return;
      }
    }
  //sub
    else if (opcode == 0x33 && funct3 == 0x0 && funct7 == 0x20) {
      idex_next.valid = true;
      idex_next.pc = ifid_cur.pc;
      idex_next.instr = instr;
      idex_next.kind = InstrKind::SUB;
      idex_next.rd = static_cast<uint8_t>(rd);
      idex_next.rs1 = static_cast<uint8_t>(rs1);
      idex_next.rs2 = static_cast<uint8_t>(rs2);
      idex_next.rs1_val = regs[rs1];
      idex_next.rs2_val = regs[rs2];
      idex_next.alu_op = AluOp::SUB;   
      idex_next.reg_write = true;
      idex_next.wb_sel = WbSel::ALU;
    }

  //lw
    else if (opcode == 0x03 && funct3 == 0x2) { 
      idex_next.valid = true;
      idex_next.pc = ifid_cur.pc;
      idex_next.instr = instr;
      idex_next.kind = InstrKind::LW;
      idex_next.rd = static_cast<uint8_t>(rd);
      idex_next.rs1 = static_cast<uint8_t>(rs1);
      idex_next.rs1_val = regs[rs1];
      idex_next.imm = signExtend12(imm12);
      idex_next.alu_op = AluOp::ADD;
      idex_next.mem_read = true;
      idex_next.reg_write = true;
      idex_next.wb_sel = WbSel::MEM;
    }

  //lb
    else if (opcode == 0x03 && funct3 == 0x0) {   
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::LB;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs1_val = regs[rs1];
        idex_next.imm = signExtend12(imm12);
        idex_next.alu_op = AluOp::ADD;
        idex_next.mem_read = true;
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::MEM;
    }

  //lh
    else if (opcode == 0x03 && funct3 == 0x1) {   
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::LH;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs1_val = regs[rs1];
        idex_next.imm = signExtend12(imm12);
        idex_next.alu_op = AluOp::ADD;
        idex_next.mem_read = true;
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::MEM;
    }

  //lbu
    else if (opcode == 0x03 && funct3 == 0x4) {   
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::LBU;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs1_val = regs[rs1];
        idex_next.imm = signExtend12(imm12);
        idex_next.alu_op = AluOp::ADD;
        idex_next.mem_read = true;
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::MEM;
    }

   //lhu
    else if (opcode == 0x03 && funct3 == 0x5) {   
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::LHU;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs1_val = regs[rs1];
        idex_next.imm = signExtend12(imm12);
        idex_next.alu_op = AluOp::ADD;
        idex_next.mem_read = true;
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::MEM;
    } 

  //sw
    else if (opcode == 0x23 && funct3 == 0x2) {   // sw
      idex_next.valid = true;
      idex_next.pc = ifid_cur.pc;
      idex_next.instr = instr;
      idex_next.kind = InstrKind::SW;

      idex_next.rs1 = static_cast<uint8_t>(rs1);
      idex_next.rs2 = static_cast<uint8_t>(rs2);

      idex_next.rs1_val = regs[rs1];
      idex_next.rs2_val = regs[rs2];

      idex_next.imm = signExtend12(imm_s);
      idex_next.alu_op = AluOp::ADD;

      idex_next.mem_write = true;
      idex_next.reg_write = false;
      idex_next.wb_sel = WbSel::NONE;
    }

  //sb
    else if (opcode == 0x23 && funct3 == 0x0) {   
      idex_next.valid = true;
      idex_next.pc = ifid_cur.pc;
      idex_next.instr = instr;
      idex_next.kind = InstrKind::SB;

      idex_next.rs1 = static_cast<uint8_t>(rs1);
      idex_next.rs2 = static_cast<uint8_t>(rs2);
      idex_next.rs1_val = regs[rs1];
      idex_next.rs2_val = regs[rs2];

      idex_next.imm = signExtend12(imm_s);
      idex_next.alu_op = AluOp::ADD;
      idex_next.mem_write = true;
      idex_next.reg_write = false;
      idex_next.wb_sel = WbSel::NONE;
    }

  //sh
    else if (opcode == 0x23 && funct3 == 0x1) {   
      idex_next.valid = true;
      idex_next.pc = ifid_cur.pc;
      idex_next.instr = instr;
      idex_next.kind = InstrKind::SH;

      idex_next.rs1 = static_cast<uint8_t>(rs1);
      idex_next.rs2 = static_cast<uint8_t>(rs2);
      idex_next.rs1_val = regs[rs1];
      idex_next.rs2_val = regs[rs2];

      idex_next.imm = signExtend12(imm_s);
      idex_next.alu_op = AluOp::ADD;
      idex_next.mem_write = true;
      idex_next.reg_write = false;
      idex_next.wb_sel = WbSel::NONE;
    }

  // b-TYPE
      else if (opcode == 0x63) {   
          idex_next.valid = true;
          idex_next.pc = ifid_cur.pc;
          idex_next.instr = instr;

          switch (funct3) {
            case 0x0: idex_next.kind = InstrKind::BEQ;  break;
            case 0x1: idex_next.kind = InstrKind::BNE;  break;
            case 0x4: idex_next.kind = InstrKind::BLT;  break;
            case 0x5: idex_next.kind = InstrKind::BGE;  break;
            case 0x6: idex_next.kind = InstrKind::BLTU; break;
            case 0x7: idex_next.kind = InstrKind::BGEU; break;
            default:
              idex_next = {};
              return;
          }

          idex_next.rs1 = static_cast<uint8_t>(rs1);
          idex_next.rs2 = static_cast<uint8_t>(rs2);
          idex_next.rs1_val = regs[rs1];
          idex_next.rs2_val = regs[rs2];
          idex_next.imm = signExtend13(imm_b);

          idex_next.alu_op = AluOp::NONE;
          idex_next.reg_write = false;
          idex_next.mem_read = false;
          idex_next.mem_write = false;
          idex_next.wb_sel = WbSel::NONE;
      }


//JALR
      else if (opcode == 0x67 && funct3 == 0x0) {   
          idex_next.valid = true;
          idex_next.pc = ifid_cur.pc;
          idex_next.instr = instr;
          idex_next.kind = InstrKind::JALR;

          idex_next.rd = static_cast<uint8_t>(rd);
          idex_next.rs1 = static_cast<uint8_t>(rs1);
          idex_next.rs1_val = regs[rs1];
          idex_next.imm = signExtend12(imm12);

          idex_next.alu_op = AluOp::NONE;
          idex_next.reg_write = true;
          idex_next.mem_read = false;
          idex_next.mem_write = false;
          idex_next.wb_sel = WbSel::ALU;   // 走 ALU 写回 pc+4
      }

//JAL
    else if (opcode == 0x6F) {   // jal
      idex_next.valid = true;
      idex_next.pc = ifid_cur.pc;
      idex_next.instr = instr;
      idex_next.kind = InstrKind::JAL;

      idex_next.rd = static_cast<uint8_t>(rd);
      idex_next.imm = signExtend21(imm_j);

      idex_next.alu_op = AluOp::NONE;     
      idex_next.reg_write = true;
      idex_next.mem_read = false;
      idex_next.mem_write = false;
      idex_next.wb_sel = WbSel::ALU;
    }

    // lui
    if (opcode == 0x37) {
      idex_next.valid = true;
      idex_next.pc = ifid_cur.pc;
      idex_next.instr = instr;
      idex_next.kind = InstrKind::LUI;
      idex_next.rd = static_cast<uint8_t>(rd);
      idex_next.imm = static_cast<int32_t>(imm_u);
      idex_next.reg_write = true;
      idex_next.mem_read = false;
      idex_next.mem_write = false;
      idex_next.wb_sel = WbSel::ALU;
    }
// auipc
    else if (opcode == 0x17) {
      idex_next.valid = true;
      idex_next.pc = ifid_cur.pc;
      idex_next.instr = instr;
      idex_next.kind = InstrKind::AUIPC;
      idex_next.rd = static_cast<uint8_t>(rd);
      idex_next.imm = static_cast<int32_t>(imm_u);
      idex_next.reg_write = true;
      idex_next.mem_read = false;
      idex_next.mem_write = false;
      idex_next.wb_sel = WbSel::ALU;
    }

//--------------------I-type------------------------------//

 // xori
    else if (opcode == 0x13 && funct3 == 0x4) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::XORI;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs1_val = regs[rs1];
        idex_next.imm = signExtend12(imm12);
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::ALU;
    }   

// ori
    else if (opcode == 0x13 && funct3 == 0x6) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::ORI;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs1_val = regs[rs1];
        idex_next.imm = signExtend12(imm12);
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::ALU;
    }

// andi
    else if (opcode == 0x13 && funct3 == 0x7) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::ANDI;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs1_val = regs[rs1];
        idex_next.imm = signExtend12(imm12);
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::ALU;
    }

//slti
    else if (opcode == 0x13 && funct3 == 0x2) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::SLTI;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs1_val = regs[rs1];
        idex_next.imm = signExtend12(imm12);
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::ALU;
    }

//sltiu
    else if (opcode == 0x13 && funct3 == 0x3) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::SLTIU;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs1_val = regs[rs1];
        idex_next.imm = signExtend12(imm12);
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::ALU;
    }

// slli
    else if (opcode == 0x13 && funct3 == 0x1 && funct7 == 0x00) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::SLLI;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs1_val = regs[rs1];
        idex_next.imm = static_cast<int32_t>(rs2); // shamt 就在 instr[24:20]
        idex_next.alu_op = AluOp::NONE;
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::ALU;
    }
// srli
    else if (opcode == 0x13 && funct3 == 0x5 && funct7 == 0x00) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::SRLI;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs1_val = regs[rs1];
        idex_next.imm = static_cast<int32_t>(rs2); // shamt
        idex_next.alu_op = AluOp::NONE;
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::ALU;
    }
// srai
    else if (opcode == 0x13 && funct3 == 0x5 && funct7 == 0x20) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::SRAI;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs1_val = regs[rs1];
        idex_next.imm = static_cast<int32_t>(rs2); // shamt
        idex_next.alu_op = AluOp::NONE;
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::ALU;
    }




//-------------------------------R-type------------------------------//

// xor
    else if (opcode == 0x33 && funct3 == 0x4 && funct7 == 0x00) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::XOR;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs2 = static_cast<uint8_t>(rs2);
        idex_next.rs1_val = regs[rs1];
        idex_next.rs2_val = regs[rs2];
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::ALU;
    }

//or
    else if (opcode == 0x33 && funct3 == 0x6 && funct7 == 0x00) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::OR;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs2 = static_cast<uint8_t>(rs2);
        idex_next.rs1_val = regs[rs1];
        idex_next.rs2_val = regs[rs2];
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::ALU;
    }

//and
    else if (opcode == 0x33 && funct3 == 0x7 && funct7 == 0x00) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::AND;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs2 = static_cast<uint8_t>(rs2);
        idex_next.rs1_val = regs[rs1];
        idex_next.rs2_val = regs[rs2];
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::ALU;
    }

//slt
    else if (opcode == 0x33 && funct3 == 0x2 && funct7 == 0x00) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::SLT;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs2 = static_cast<uint8_t>(rs2);
        idex_next.rs1_val = regs[rs1];
        idex_next.rs2_val = regs[rs2];
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::ALU;
    }

//stlu
    else if (opcode == 0x33 && funct3 == 0x3 && funct7 == 0x00) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::SLTU;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs2 = static_cast<uint8_t>(rs2);
        idex_next.rs1_val = regs[rs1];
        idex_next.rs2_val = regs[rs2];
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::ALU;
    }

// sll
    else if (opcode == 0x33 && funct3 == 0x1 && funct7 == 0x00) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::SLL;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs2 = static_cast<uint8_t>(rs2);
        idex_next.rs1_val = regs[rs1];
        idex_next.rs2_val = regs[rs2];
        idex_next.alu_op = AluOp::NONE;
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::ALU;
    }
// srl
    else if (opcode == 0x33 && funct3 == 0x5 && funct7 == 0x00) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::SRL;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs2 = static_cast<uint8_t>(rs2);
        idex_next.rs1_val = regs[rs1];
        idex_next.rs2_val = regs[rs2];
        idex_next.alu_op = AluOp::NONE;
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::ALU;
    }
// sra
    else if (opcode == 0x33 && funct3 == 0x5 && funct7 == 0x20) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::SRA;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.rs2 = static_cast<uint8_t>(rs2);
        idex_next.rs1_val = regs[rs1];
        idex_next.rs2_val = regs[rs2];
        idex_next.alu_op = AluOp::NONE;
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::ALU;
    }
   // fence / fence.i.  In Spirit, FENCE.I only resets the BTB.  This model's
   // branch predictor is intentionally deferred, so the architectural pipe
   // behavior is the same as FENCE while retaining a distinct instruction.
    else if (opcode == 0x0F && funct3 == 0x0) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::FENCE;

        idex_next.alu_op = AluOp::NONE;
        idex_next.reg_write = false;
        idex_next.mem_read = false;
        idex_next.mem_write = false;
        idex_next.wb_sel = WbSel::NONE;
    }
    else if (opcode == 0x0F && funct3 == 0x1) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::FENCE_I;
        idex_next.wb_sel = WbSel::NONE;
    }

    // Zicsr.  CSRU is connected directly to Decoder/IDU in the RTL, so the
    // old value is captured and the write side effect occurs in ID.
    else if (opcode == 0x73 && funct3 != 0 && funct3 != 0x4) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.rd = static_cast<uint8_t>(rd);
        idex_next.rs1 = static_cast<uint8_t>(rs1);
        idex_next.csr_addr = static_cast<uint16_t>(imm12);
        idex_next.reg_write = true;
        idex_next.wb_sel = WbSel::ALU;

        const bool immediate = (funct3 & 0x4) != 0;
        const uint32_t operand = immediate ? rs1 :
            decodeForwardedReg(static_cast<uint8_t>(rs1));
        idex_next.csr_wdata = operand;

        switch (funct3) {
          case 0x1:
            idex_next.kind = InstrKind::CSRRW;
            idex_next.csr_write_type = CsrWriteType::WRITE;
            idex_next.csr_read = rd != 0;
            idex_next.csr_write = true;
            break;
          case 0x2:
            idex_next.kind = InstrKind::CSRRS;
            idex_next.csr_write_type = CsrWriteType::SET;
            idex_next.csr_read = true;
            idex_next.csr_write = rs1 != 0;
            break;
          case 0x3:
            idex_next.kind = InstrKind::CSRRC;
            idex_next.csr_write_type = CsrWriteType::CLEAR;
            idex_next.csr_read = true;
            idex_next.csr_write = rs1 != 0;
            break;
          case 0x5:
            idex_next.kind = InstrKind::CSRRWI;
            idex_next.csr_write_type = CsrWriteType::WRITE;
            idex_next.csr_read = rd != 0;
            idex_next.csr_write = true;
            break;
          case 0x6:
            idex_next.kind = InstrKind::CSRRSI;
            idex_next.csr_write_type = CsrWriteType::SET;
            idex_next.csr_read = true;
            idex_next.csr_write = rs1 != 0;
            break;
          case 0x7:
            idex_next.kind = InstrKind::CSRRCI;
            idex_next.csr_write_type = CsrWriteType::CLEAR;
            idex_next.csr_read = true;
            idex_next.csr_write = rs1 != 0;
            break;
          default:
            idex_next = {};
            return;
        }

        idex_next.csr_rdata = idex_next.csr_read ?
            readCsr(idex_next.csr_addr, operand) : 0;
        if (idex_next.csr_read) {
            applyCsrReadSideEffects(idex_next.csr_addr, operand);
        }
        if (idex_next.csr_write) {
            writeCsr(idex_next.csr_addr, operand,
                     idex_next.csr_write_type);
        }
    }

  // Privileged/system encodings follow Decoder.sv exactly.  In particular,
  // ECALL has exception type zero in this RTL and behaves as a normal no-op.
    else if (instr == 0x00000073u || instr == 0x00100073u ||
             instr == 0x30200073u || instr == 0x10500073u) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;

        if (instr == 0x00000073u) {
            idex_next.kind = InstrKind::ECALL;
        } else if (instr == 0x00100073u) {
            idex_next.kind = InstrKind::EBREAK;
        } else if (instr == 0x30200073u) {
            idex_next.kind = InstrKind::MRET;
        } else {
            idex_next.kind = InstrKind::WFI;
        }

        idex_next.alu_op = AluOp::NONE;
        idex_next.reg_write = false;
        idex_next.mem_read = false;
        idex_next.mem_write = false;
        idex_next.wb_sel = WbSel::NONE;
    }

    // CIDU.sv asserts r_id_instr_valid for every full-width instruction
    // (instr[1:0] == 2'b11), even when Decoder.sv recognizes no operation.
    // Such an encoding therefore occupies the IE/IC/WB pipeline and retires
    // as a side-effect-free instruction.  Unsupported compressed encodings
    // were rejected above and intentionally do not take this path.
    if (!idex_next.valid && instr_len == 4) {
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;
        idex_next.kind = InstrKind::INVALID;
        idex_next.alu_op = AluOp::NONE;
        idex_next.reg_write = false;
        idex_next.mem_read = false;
        idex_next.mem_write = false;
        idex_next.wb_sel = WbSel::NONE;
    }

    if (idex_next.valid) {
        idex_next.instr_len = instr_len;

        // JCU resolves branch and jump targets in ID.  Use the same IE/IC
        // bypass sources as CSR decode, so a producer immediately ahead of
        // the control-flow instruction does not add a bubble.
        if (!csr_debug_mode && !takeInterrupt && !takeDebug) {
            bool redirect = false;
            uint32_t target = 0;
            const uint32_t lhs = decodeForwardedReg(idex_next.rs1);
            const uint32_t rhs = decodeForwardedReg(idex_next.rs2);

            switch (idex_next.kind) {
              case InstrKind::BEQ:  redirect = lhs == rhs; break;
              case InstrKind::BNE:  redirect = lhs != rhs; break;
              case InstrKind::BLT:
                redirect = static_cast<int32_t>(lhs) <
                           static_cast<int32_t>(rhs);
                break;
              case InstrKind::BGE:
                redirect = static_cast<int32_t>(lhs) >=
                           static_cast<int32_t>(rhs);
                break;
              case InstrKind::BLTU: redirect = lhs < rhs; break;
              case InstrKind::BGEU: redirect = lhs >= rhs; break;
              case InstrKind::JAL:
                redirect = true;
                break;
              case InstrKind::JALR:
                redirect = true;
                target = static_cast<uint32_t>(
                    static_cast<int32_t>(lhs) + idex_next.imm);
                break;
              default:
                break;
            }

            if (redirect) {
                if (idex_next.kind != InstrKind::JALR) {
                    target = static_cast<uint32_t>(
                        static_cast<int32_t>(idex_next.pc) + idex_next.imm);
                }
                const uint32_t sequentialPc =
                    idex_next.pc + idex_next.instr_len;
                if (target != sequentialPc) {
                    jcu_redirect_pending = true;
                    jcu_redirect_target_pending = target;
                    ++flush_count;
                }
            }
        }

        // Interrupt entry is resolved in ID in Spirit.  The instruction
        // currently in ID is allowed to advance; only younger fetch state is
        // redirected, and mepc receives that instruction's sequential PC.
        if (takeInterrupt) {
            enterTrap(idex_next.pc + idex_next.instr_len,
                      pendingInterruptCode, true);
            ++flush_count;
        } else if (takeDebug) {
            enterDebug(idex_next.pc + idex_next.instr_len,
                       debug_step_halt_requested ? 4 : 3);
            ++flush_count;
        }
    }
}

} // namespace gem5
