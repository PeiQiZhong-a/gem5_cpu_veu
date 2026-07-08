#include "brs/pipeline/pipeline_core.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace
{

TEST(PipelineVeuWritebackTest, WritesVeuResultFromMemwbToRd)
{
    PipelineCore core;
    core.reset(0x80000000u, 0x80000000u);
    core.fetchInstr = [](uint32_t, uint32_t &) { return false; };

    core.memwb_cur.valid = true;
    core.memwb_cur.pc = 0x80000000u;
    core.memwb_cur.instr = 0x0000000bu;
    core.memwb_cur.kind = InstrKind::VEU;
    core.memwb_cur.rd = 11;
    core.memwb_cur.reg_write = true;
    core.memwb_cur.wb_sel = WbSel::VEU;
    core.memwb_cur.alu_result = 0xc001cafeu;

    core.stepOneCycle();

    EXPECT_EQ(core.getReg(11), 0xc001cafeu);
    EXPECT_EQ(core.getRetiredInstCount(), 1u);
}

TEST(PipelineVeuWritebackTest, KeepsX0HardwiredForVeuWriteback)
{
    PipelineCore core;
    core.reset(0x80000000u, 0x80000000u);
    core.fetchInstr = [](uint32_t, uint32_t &) { return false; };
    core.regs[0] = 0xdeadbeefu;

    core.memwb_cur.valid = true;
    core.memwb_cur.pc = 0x80000000u;
    core.memwb_cur.instr = 0x0000000bu;
    core.memwb_cur.kind = InstrKind::VEU;
    core.memwb_cur.rd = 0;
    core.memwb_cur.reg_write = true;
    core.memwb_cur.wb_sel = WbSel::VEU;
    core.memwb_cur.alu_result = 0xc001cafeu;

    core.stepOneCycle();

    EXPECT_EQ(core.getReg(0), 0u);
    EXPECT_EQ(core.getRetiredInstCount(), 1u);
}

} // namespace
} // namespace gem5
