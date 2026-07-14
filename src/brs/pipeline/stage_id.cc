#include "brs/pipeline/pipeline_core.hh"
#include "brs/pipeline/compressed_decode.hh"
#include "brs/pipeline/veu_decode.hh"

namespace gem5
{

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

  // addi 
    if (opcode == 0x13 && funct3 == 0x0) {
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
   // fence
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

  //ECALL / EBREAK
    else if (opcode == 0x73 && funct3 == 0x0) {  
        idex_next.valid = true;
        idex_next.pc = ifid_cur.pc;
        idex_next.instr = instr;

        if (imm12 == 0x000) {
            idex_next.kind = InstrKind::ECALL;
        } else if (imm12 == 0x001) {
            idex_next.kind = InstrKind::EBREAK;
        } else {
            idex_next = {};
            return;
        }

        idex_next.alu_op = AluOp::NONE;
        idex_next.reg_write = false;
        idex_next.mem_read = false;
        idex_next.mem_write = false;
        idex_next.wb_sel = WbSel::NONE;
    }

    if (idex_next.valid) {
        idex_next.instr_len = instr_len;
    }
}

} // namespace gem5
