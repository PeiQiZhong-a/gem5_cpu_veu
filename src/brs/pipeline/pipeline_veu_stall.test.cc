#include "brs/pipeline/pipeline_core.hh"

#include <cstdint>

#include <gtest/gtest.h>

namespace gem5
{
namespace
{

constexpr uint32_t
encodeAddi(uint8_t rd, uint8_t rs1, uint16_t imm)
{
    return ((static_cast<uint32_t>(imm) & 0xfff) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (0u << 12) |
           (static_cast<uint32_t>(rd) << 7) |
           0x13u;
}

IDEX
makeVeuInExecute(
    brs::VeuInstruction operation,
    uint8_t rd,
    uint32_t rs1Value,
    uint32_t rs2Value,
    uint32_t rs3Value = 0)
{
    IDEX idex;
    idex.valid = true;
    idex.pc = 0x80000000u;
    idex.instr = 0xfeed006bu;
    idex.kind = InstrKind::VEU;
    idex.rd = rd;
    idex.rs1 = 1;
    idex.rs2 = 2;
    idex.rs3 = 3;
    idex.rs1_val = rs1Value;
    idex.rs2_val = rs2Value;
    idex.rs3_val = rs3Value;
    idex.veu_operation = operation;
    idex.veu_csr_addr =
        static_cast<uint16_t>(brs::VeuCsr::VectorLength);
    idex.veu_csr_read = true;
    idex.veu_csr_write = true;
    idex.veu_write_type = brs::VeuWriteType::VectorStart;
    idex.veu_start = brs::veuStartMask(operation);
    idex.reg_write = rd != 0;
    idex.wb_sel = rd != 0 ? WbSel::VEU : WbSel::NONE;
    return idex;
}

IFID
makeYoungerAddiInDecode()
{
    IFID ifid;
    ifid.valid = true;
    ifid.pc = 0x80000004u;
    ifid.instr = encodeAddi(5, 0, 7);
    ifid.instr_len = 4;
    return ifid;
}

void
disableFetch(PipelineCore &core)
{
    core.fetchInstr = [](uint32_t, uint32_t &) { return false; };
}

TEST(PipelineVeuStallTest, HoldsFetchAndDecodeWhileOneShotCbuWaits)
{
    PipelineCore core;
    core.reset(0x80000000u, 0x80000000u);
    core.configureFakeVeu(4, 0xc001cafeu);
    disableFetch(core);

    core.idex_cur = makeVeuInExecute(
        brs::VeuInstruction::Add, 9, 0x11111111u, 0x22222222u);
    core.ifid_cur = makeYoungerAddiInDecode();

    const IDEX originalIdex = core.idex_cur;
    const IFID originalIfid = core.ifid_cur;

    core.stepOneCycle();
    EXPECT_TRUE(core.spiritExecuteStalled());
    EXPECT_EQ(core.getVeuIssueCount(), 1u);
    EXPECT_EQ(core.getFakeVeuAcceptedRequestCount(), 0u);
    EXPECT_EQ(core.idex_cur.instr, originalIdex.instr);
    EXPECT_EQ(core.ifid_cur.instr, originalIfid.instr);
    EXPECT_FALSE(core.exmem_cur.valid);

    core.stepOneCycle();
    EXPECT_TRUE(core.spiritExecuteStalled());
    EXPECT_EQ(core.getVeuIssueCount(), 1u);
    EXPECT_EQ(core.getFakeVeuAcceptedRequestCount(), 1u);
    EXPECT_EQ(core.idex_cur.instr, originalIdex.instr);
    EXPECT_EQ(core.ifid_cur.instr, originalIfid.instr);

    for (int i = 0; i < 8 && core.getVeuCompleteCount() == 0; ++i) {
        core.stepOneCycle();
        if (core.getVeuCompleteCount() == 0) {
            EXPECT_TRUE(core.spiritExecuteStalled());
            EXPECT_EQ(core.idex_cur.instr, originalIdex.instr);
            EXPECT_EQ(core.ifid_cur.instr, originalIfid.instr);
        }
    }

    EXPECT_EQ(core.getVeuCompleteCount(), 1u);
    EXPECT_FALSE(core.spiritExecuteStalled());
    EXPECT_TRUE(core.exmem_cur.valid);
    EXPECT_EQ(core.exmem_cur.kind, InstrKind::VEU);
    EXPECT_EQ(core.exmem_cur.alu_result, 0xc001cafeu);

    // The younger ADDI is decoded only after the CBU completion cycle,
    // matching Spirit's "stall until cbu_complete" behavior.
    EXPECT_TRUE(core.idex_cur.valid);
    EXPECT_EQ(core.idex_cur.kind, InstrKind::ADDI);
    EXPECT_EQ(core.idex_cur.pc, originalIfid.pc);
}

TEST(PipelineVeuStallTest, KeepsVmaddRs3StableForSecondCbuRequest)
{
    PipelineCore core;
    core.reset(0x80000000u, 0x80000000u);
    core.configureFakeVeu(2, 0x12345678u);
    disableFetch(core);

    core.idex_cur = makeVeuInExecute(
        brs::VeuInstruction::MultiplyAdd,
        10,
        0xaaaaaaaau,
        0xbbbbbbbbu,
        0x33333333u);
    core.ifid_cur = makeYoungerAddiInDecode();

    for (int i = 0; i < 12 && core.getFakeVeuAcceptedRequestCount() < 2; ++i) {
        core.stepOneCycle();
        if (core.getFakeVeuAcceptedRequestCount() < 2) {
            EXPECT_TRUE(core.idex_cur.valid);
            EXPECT_EQ(core.idex_cur.kind, InstrKind::VEU);
            EXPECT_EQ(core.idex_cur.rs3_val, 0x33333333u);
        }
    }

    ASSERT_EQ(core.getFakeVeuAcceptedRequestCount(), 2u);
    EXPECT_EQ(
        core.getFakeVeuLastRequest().writeData,
        brs::packVeuOperands(0x33333333u, 0x33333333u));

    for (int i = 0; i < 12 && core.getVeuCompleteCount() == 0; ++i) {
        core.stepOneCycle();
    }

    EXPECT_EQ(core.getVeuCompleteCount(), 1u);
    EXPECT_FALSE(core.spiritExecuteStalled());
    EXPECT_TRUE(core.exmem_cur.valid);
    EXPECT_EQ(core.exmem_cur.kind, InstrKind::VEU);
}

} // namespace
} // namespace gem5
