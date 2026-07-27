#ifndef __BRS_PIPELINE_REGS_HH__
#define __BRS_PIPELINE_REGS_HH__

#include <cstdint>
#include "brs/pipeline/pipeline_types.hh"
#include "brs/sau/sau_protocol.hh"
#include "brs/veu/veu_protocol.hh"

namespace gem5
{

struct IFID
{
    bool valid = false;
    uint32_t pc = 0;
    uint32_t instr = 0;
    uint8_t instr_len = 4;
    bool debug_instr = false;
};

struct IDEX
{
    bool valid = false;
    uint32_t pc = 0;
    uint32_t instr = 0;
    uint8_t instr_len = 4;

    InstrKind kind = InstrKind::NOP;

    uint8_t rd = 0;
    uint8_t rs1 = 0;
    uint8_t rs2 = 0;
    uint8_t rs3 = 0;

    bool rd_fp = false;
    bool rs1_fp = false;
    bool rs2_fp = false;
    bool rs3_fp = false;

    uint32_t rs1_val = 0;
    uint32_t rs2_val = 0;
    uint32_t rs3_val = 0;
    int32_t imm = 0;

    uint16_t csr_addr = 0;
    uint32_t csr_rdata = 0;
    uint32_t csr_wdata = 0;
    CsrWriteType csr_write_type = CsrWriteType::WRITE;
    bool csr_read = false;
    bool csr_write = false;

    brs::VeuInstruction veu_operation = brs::VeuInstruction::Unknown;
    uint16_t veu_csr_addr = 0;
    bool veu_csr_read = false;
    bool veu_csr_write = false;
    brs::VeuWriteType veu_write_type = brs::VeuWriteType::Write;
    uint32_t veu_start = 0;

    brs::SauInstruction sau_operation = brs::SauInstruction::Unknown;
    uint16_t sau_csr_addr = 0;
    bool sau_csr_read = false;
    bool sau_csr_write = false;
    brs::SauWriteType sau_write_type = brs::SauWriteType::Write;

    AluOp alu_op = AluOp::NONE;
    bool mdu_op_is_div = false;
    bool mdu_op1_is_signed = false;
    bool mdu_op2_is_signed = false;
    bool mdu_output_is_high = false;
    bool reg_write = false;
    bool mem_read = false;
    bool mem_write = false;
    WbSel wb_sel = WbSel::NONE;
};

struct EXMEM
{
    bool valid = false;
    uint32_t pc = 0;

    InstrKind kind = InstrKind::NOP;

    uint8_t rd = 0;
    bool rd_fp = false;
    uint32_t alu_result = 0;
    uint32_t mem_data = 0;
    uint32_t store_data = 0;
    uint32_t instr = 0;
    uint8_t instr_len = 4;

    bool reg_write = false;
    bool mem_read = false;
    bool mem_write = false;
    WbSel wb_sel = WbSel::NONE;
};

struct MEMWB
{
    bool valid = false;
    uint32_t pc = 0;

    InstrKind kind = InstrKind::NOP;

    uint8_t rd = 0;
    bool rd_fp = false;
    uint32_t alu_result = 0;
    uint32_t mem_data = 0;
    uint32_t instr = 0;
    uint8_t instr_len = 4;

    bool reg_write = false;
    WbSel wb_sel = WbSel::NONE;
};

} // namespace gem5

#endif
