#include "brs/pipeline/pipeline_core.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace
{

uint32_t
encodeM(uint8_t funct3)
{
    return (0x01u << 25) | (2u << 20) | (1u << 15) |
        (static_cast<uint32_t>(funct3) << 12) | (5u << 7) | 0x33u;
}

IDEX
makeMInstr(InstrKind kind, uint8_t funct3, uint32_t lhs, uint32_t rhs)
{
    IDEX reg = {};
    reg.valid = true;
    reg.pc = 0x80000000u;
    reg.instr = encodeM(funct3);
    reg.instr_len = 4;
    reg.kind = kind;
    reg.rd = 5;
    reg.rs1 = 1;
    reg.rs2 = 2;
    reg.rs1_val = lhs;
    reg.rs2_val = rhs;
    reg.reg_write = true;
    reg.wb_sel = WbSel::ALU;

    switch (kind) {
      case InstrKind::MUL:
        reg.alu_op = AluOp::MUL;
        reg.mdu_op1_is_signed = true;
        reg.mdu_op2_is_signed = true;
        break;
      case InstrKind::MULH:
        reg.alu_op = AluOp::MULH;
        reg.mdu_op1_is_signed = true;
        reg.mdu_op2_is_signed = true;
        reg.mdu_output_is_high = true;
        break;
      case InstrKind::MULHSU:
        reg.alu_op = AluOp::MULHSU;
        reg.mdu_op1_is_signed = true;
        reg.mdu_output_is_high = true;
        break;
      case InstrKind::MULHU:
        reg.alu_op = AluOp::MULHU;
        reg.mdu_output_is_high = true;
        break;
      case InstrKind::DIV:
        reg.alu_op = AluOp::DIV;
        reg.mdu_op_is_div = true;
        reg.mdu_op1_is_signed = true;
        reg.mdu_op2_is_signed = true;
        break;
      case InstrKind::DIVU:
        reg.alu_op = AluOp::DIVU;
        reg.mdu_op_is_div = true;
        break;
      case InstrKind::REM:
        reg.alu_op = AluOp::REM;
        reg.mdu_op_is_div = true;
        reg.mdu_op1_is_signed = true;
        reg.mdu_op2_is_signed = true;
        reg.mdu_output_is_high = true;
        break;
      case InstrKind::REMU:
        reg.alu_op = AluOp::REMU;
        reg.mdu_op_is_div = true;
        reg.mdu_output_is_high = true;
        break;
      default:
        break;
    }

    return reg;
}

uint32_t
executeM(InstrKind kind, uint8_t funct3, uint32_t lhs, uint32_t rhs)
{
    PipelineCore core;
    core.reset(0x80000000u, 0x80000000u);
    core.fetchInstr = [](uint32_t, uint32_t &) { return false; };
    core.idex_cur = makeMInstr(kind, funct3, lhs, rhs);

    core.stageEX();
    while (core.mduStalled()) {
        core.mdu_stall = false;
        core.stageEX();
    }
    EXPECT_TRUE(core.exmem_next.valid);
    EXPECT_TRUE(core.exmem_next.reg_write);
    EXPECT_EQ(core.exmem_next.rd, 5);
    EXPECT_EQ(core.exmem_next.wb_sel, WbSel::ALU);
    return core.exmem_next.alu_result;
}

TEST(PipelineRv32mTest, DecodesAllMExtensionFunct3Values)
{
    const struct {
        uint8_t funct3;
        InstrKind kind;
    } cases[] = {
        {0x0, InstrKind::MUL},
        {0x1, InstrKind::MULH},
        {0x2, InstrKind::MULHSU},
        {0x3, InstrKind::MULHU},
        {0x4, InstrKind::DIV},
        {0x5, InstrKind::DIVU},
        {0x6, InstrKind::REM},
        {0x7, InstrKind::REMU},
    };

    for (const auto &testCase : cases) {
        PipelineCore core;
        core.reset(0x80000000u, 0x80000000u);
        core.regs[1] = 0x12345678u;
        core.regs[2] = 0xfedcba98u;
        core.ifid_cur.valid = true;
        core.ifid_cur.pc = 0x80000000u;
        core.ifid_cur.instr = encodeM(testCase.funct3);
        core.ifid_cur.instr_len = 4;

        core.stageID();

        EXPECT_TRUE(core.idex_next.valid);
        EXPECT_EQ(core.idex_next.kind, testCase.kind);
        EXPECT_EQ(core.idex_next.rd, 5);
        EXPECT_EQ(core.idex_next.rs1, 1);
        EXPECT_EQ(core.idex_next.rs2, 2);
        EXPECT_EQ(core.idex_next.rs1_val, 0x12345678u);
        EXPECT_EQ(core.idex_next.rs2_val, 0xfedcba98u);
        EXPECT_TRUE(core.idex_next.reg_write);
        EXPECT_EQ(core.idex_next.wb_sel, WbSel::ALU);

        switch (testCase.kind) {
          case InstrKind::MUL:
            EXPECT_EQ(core.idex_next.alu_op, AluOp::MUL);
            EXPECT_TRUE(core.idex_next.mdu_op1_is_signed);
            EXPECT_TRUE(core.idex_next.mdu_op2_is_signed);
            EXPECT_FALSE(core.idex_next.mdu_output_is_high);
            EXPECT_FALSE(core.idex_next.mdu_op_is_div);
            break;
          case InstrKind::MULH:
            EXPECT_EQ(core.idex_next.alu_op, AluOp::MULH);
            EXPECT_TRUE(core.idex_next.mdu_op1_is_signed);
            EXPECT_TRUE(core.idex_next.mdu_op2_is_signed);
            EXPECT_TRUE(core.idex_next.mdu_output_is_high);
            EXPECT_FALSE(core.idex_next.mdu_op_is_div);
            break;
          case InstrKind::MULHSU:
            EXPECT_EQ(core.idex_next.alu_op, AluOp::MULHSU);
            EXPECT_TRUE(core.idex_next.mdu_op1_is_signed);
            EXPECT_FALSE(core.idex_next.mdu_op2_is_signed);
            EXPECT_TRUE(core.idex_next.mdu_output_is_high);
            EXPECT_FALSE(core.idex_next.mdu_op_is_div);
            break;
          case InstrKind::MULHU:
            EXPECT_EQ(core.idex_next.alu_op, AluOp::MULHU);
            EXPECT_FALSE(core.idex_next.mdu_op1_is_signed);
            EXPECT_FALSE(core.idex_next.mdu_op2_is_signed);
            EXPECT_TRUE(core.idex_next.mdu_output_is_high);
            EXPECT_FALSE(core.idex_next.mdu_op_is_div);
            break;
          case InstrKind::DIV:
            EXPECT_EQ(core.idex_next.alu_op, AluOp::DIV);
            EXPECT_TRUE(core.idex_next.mdu_op1_is_signed);
            EXPECT_TRUE(core.idex_next.mdu_op2_is_signed);
            EXPECT_FALSE(core.idex_next.mdu_output_is_high);
            EXPECT_TRUE(core.idex_next.mdu_op_is_div);
            break;
          case InstrKind::DIVU:
            EXPECT_EQ(core.idex_next.alu_op, AluOp::DIVU);
            EXPECT_FALSE(core.idex_next.mdu_op1_is_signed);
            EXPECT_FALSE(core.idex_next.mdu_op2_is_signed);
            EXPECT_FALSE(core.idex_next.mdu_output_is_high);
            EXPECT_TRUE(core.idex_next.mdu_op_is_div);
            break;
          case InstrKind::REM:
            EXPECT_EQ(core.idex_next.alu_op, AluOp::REM);
            EXPECT_TRUE(core.idex_next.mdu_op1_is_signed);
            EXPECT_TRUE(core.idex_next.mdu_op2_is_signed);
            EXPECT_TRUE(core.idex_next.mdu_output_is_high);
            EXPECT_TRUE(core.idex_next.mdu_op_is_div);
            break;
          case InstrKind::REMU:
            EXPECT_EQ(core.idex_next.alu_op, AluOp::REMU);
            EXPECT_FALSE(core.idex_next.mdu_op1_is_signed);
            EXPECT_FALSE(core.idex_next.mdu_op2_is_signed);
            EXPECT_TRUE(core.idex_next.mdu_output_is_high);
            EXPECT_TRUE(core.idex_next.mdu_op_is_div);
            break;
          default:
            FAIL() << "unexpected M extension kind";
        }
    }
}

TEST(PipelineRv32mTest, MultiplierStallsForThreeExecuteCycles)
{
    PipelineCore core;
    core.reset(0x80000000u, 0x80000000u);
    core.idex_cur = makeMInstr(InstrKind::MUL, 0x0, 7u, 6u);

    unsigned stalls = 0;
    do {
        core.mdu_stall = false;
        core.stageEX();
        if (core.mduStalled()) {
            ++stalls;
        }
    } while (core.mduStalled());

    EXPECT_EQ(stalls, 3u);
    EXPECT_TRUE(core.exmem_next.valid);
    EXPECT_EQ(core.exmem_next.alu_result, 42u);
}

TEST(PipelineRv32mTest, DividerSkipPathStallsForOneExecuteCycle)
{
    PipelineCore core;
    core.reset(0x80000000u, 0x80000000u);
    core.idex_cur = makeMInstr(InstrKind::DIVU, 0x5, 42u, 1u);

    unsigned stalls = 0;
    do {
        core.mdu_stall = false;
        core.stageEX();
        if (core.mduStalled()) {
            ++stalls;
        }
    } while (core.mduStalled());

    EXPECT_EQ(stalls, 1u);
    EXPECT_TRUE(core.exmem_next.valid);
    EXPECT_EQ(core.exmem_next.alu_result, 42u);
}

TEST(PipelineRv32mTest, MulReturnsLowWord)
{
    EXPECT_EQ(executeM(InstrKind::MUL, 0x0, 0xfffffff0u, 3u),
              0xffffffd0u);
}

TEST(PipelineRv32mTest, MulHighVariantsReturnUpperWord)
{
    EXPECT_EQ(executeM(InstrKind::MULH, 0x1, 0xffffffffu, 2u),
              0xffffffffu);
    EXPECT_EQ(executeM(InstrKind::MULHSU, 0x2, 0xffffffffu, 2u),
              0xffffffffu);
    EXPECT_EQ(executeM(InstrKind::MULHU, 0x3, 0xffffffffu, 2u),
              0x00000001u);
}

TEST(PipelineRv32mTest, DivHandlesSignedAndUnsignedCases)
{
    EXPECT_EQ(executeM(InstrKind::DIV, 0x4, 0xfffffff9u, 3u),
              0xfffffffeu);
    EXPECT_EQ(executeM(InstrKind::DIVU, 0x5, 7u, 3u), 2u);
}

TEST(PipelineRv32mTest, DivByZeroReturnsAllOnes)
{
    EXPECT_EQ(executeM(InstrKind::DIV, 0x4, 7u, 0u), 0xffffffffu);
    EXPECT_EQ(executeM(InstrKind::DIVU, 0x5, 7u, 0u), 0xffffffffu);
}

TEST(PipelineRv32mTest, DivOverflowReturnsDividend)
{
    EXPECT_EQ(executeM(InstrKind::DIV, 0x4, 0x80000000u, 0xffffffffu),
              0x80000000u);
}

TEST(PipelineRv32mTest, RemHandlesSignedAndUnsignedCases)
{
    EXPECT_EQ(executeM(InstrKind::REM, 0x6, 0xfffffff9u, 3u),
              0xffffffffu);
    EXPECT_EQ(executeM(InstrKind::REMU, 0x7, 7u, 3u), 1u);
}

TEST(PipelineRv32mTest, RemByZeroReturnsDividend)
{
    EXPECT_EQ(executeM(InstrKind::REM, 0x6, 7u, 0u), 7u);
    EXPECT_EQ(executeM(InstrKind::REMU, 0x7, 7u, 0u), 7u);
}

TEST(PipelineRv32mTest, RemOverflowReturnsZero)
{
    EXPECT_EQ(executeM(InstrKind::REM, 0x6, 0x80000000u, 0xffffffffu),
              0u);
}

} // namespace
} // namespace gem5
