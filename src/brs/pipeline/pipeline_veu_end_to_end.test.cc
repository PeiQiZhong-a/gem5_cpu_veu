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
           (static_cast<uint32_t>(rd) << 7) |
           0x13u;
}

constexpr uint32_t
encodeVadd(uint8_t rd, uint8_t rs1, uint8_t rs2)
{
    return (static_cast<uint32_t>(rs2) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(rd) << 7) |
           brs::VeuVectorOpcode;
}

void
loadMinimalVeuProgram(PipelineCore &core)
{
    core.program.clear();
    core.program.instr_mem[0] = encodeAddi(1, 0, 0x11);
    core.program.instr_mem[1] = encodeAddi(2, 0, 0x22);
    core.program.instr_mem[2] = encodeVadd(5, 1, 2);
    core.program.instr_mem[3] = encodeAddi(6, 5, 3);
    core.program.program_words = 4;
}

void
runUntilDone(PipelineCore &core, int maxCycles = 80)
{
    for (int i = 0; i < maxCycles && !core.done(); ++i) {
        core.stepOneCycle();
    }
}

TEST(PipelineVeuEndToEndTest, RunsMinimalVeuProgramThroughWriteback)
{
    PipelineCore core;
    core.reset(0x80000000u, 0x80000010u);
    core.configureFakeVeu(4, 0xc001cafeu);
    loadMinimalVeuProgram(core);

    runUntilDone(core);

    EXPECT_TRUE(core.done());
    EXPECT_EQ(core.getVeuIssueCount(), 1u);
    EXPECT_EQ(core.getFakeVeuAcceptedRequestCount(), 1u);
    EXPECT_EQ(core.getVeuCompleteCount(), 1u);

    EXPECT_EQ(core.getReg(1), 0x11u);
    EXPECT_EQ(core.getReg(2), 0x22u);
    EXPECT_EQ(core.getReg(5), 0xc001cafeu);
    EXPECT_EQ(core.getReg(6), 0xc001cb01u);
    EXPECT_EQ(core.getRetiredInstCount(), 4u);

    EXPECT_EQ(
        core.getFakeVeuLastRequest().writeData,
        brs::packVeuOperands(0x11u, 0x22u));
    EXPECT_EQ(
        core.getFakeVeuLastRequest().veStart,
        brs::veuStartMask(brs::VeuInstruction::Add));
}

} // namespace
} // namespace gem5
