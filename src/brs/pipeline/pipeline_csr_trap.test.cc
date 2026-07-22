#include "brs/pipeline/pipeline_core.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace
{

constexpr uint32_t
encodeCsr(uint8_t funct3, uint8_t rd, uint8_t rs1OrZimm, uint16_t addr)
{
    return (static_cast<uint32_t>(addr) << 20) |
           (static_cast<uint32_t>(rs1OrZimm) << 15) |
           (static_cast<uint32_t>(funct3) << 12) |
           (static_cast<uint32_t>(rd) << 7) | 0x73;
}

void
disableFetch(PipelineCore &core)
{
    core.fetchInstr = [](uint32_t, uint32_t &) { return false; };
}

TEST(PipelineCsrTrapTest, CsrWritesInIdWithIeuForwarding)
{
    PipelineCore core;
    core.reset(0, 0x200);
    disableFetch(core);

    core.idex_cur.valid = true;
    core.idex_cur.pc = 0;
    core.idex_cur.instr = 0x00800093; // addi x1, x0, 8
    core.idex_cur.kind = InstrKind::ADDI;
    core.idex_cur.rd = 1;
    core.idex_cur.imm = 8;
    core.idex_cur.reg_write = true;
    core.idex_cur.wb_sel = WbSel::ALU;

    core.ifid_cur = {true, 4, encodeCsr(1, 5, 1, 0x300), 4};
    core.stepOneCycle();

    EXPECT_EQ(core.getCSR(0x300), 8u);
    ASSERT_TRUE(core.idexValid());
    EXPECT_EQ(core.idex_cur.kind, InstrKind::CSRRW);

    core.stepOneCycle();
    ASSERT_TRUE(core.exmemValid());
    EXPECT_EQ(core.exmem_cur.alu_result, 0x1800u);
}

TEST(PipelineCsrTrapTest, EcallDoesNotHaltButEbreakEntersTrap)
{
    PipelineCore core;
    core.reset(0, 0x200);
    disableFetch(core);

    core.idex_cur.valid = true;
    core.idex_cur.pc = 0x10;
    core.idex_cur.instr = 0x00000073;
    core.idex_cur.instr_len = 4;
    core.idex_cur.kind = InstrKind::ECALL;
    core.stepOneCycle();
    EXPECT_FALSE(core.haltRequested());
    ASSERT_TRUE(core.exmemValid());

    core.idex_cur.valid = true;
    core.idex_cur.pc = 0x20;
    core.idex_cur.instr = 0x00100073;
    core.idex_cur.instr_len = 4;
    core.idex_cur.kind = InstrKind::EBREAK;
    core.stepOneCycle();

    EXPECT_FALSE(core.haltRequested());
    EXPECT_EQ(core.getCSR(0x341), 0x24u);
    EXPECT_EQ(core.getPC(), 0x100u);
    ASSERT_TRUE(core.exmemValid());
    EXPECT_EQ(core.exmem_cur.kind, InstrKind::EBREAK);
}

TEST(PipelineCsrTrapTest, TimerInterruptIsRegisteredAndResolvedInId)
{
    PipelineCore core;
    core.reset(0, 0x200);
    disableFetch(core);
    core.writeCsr(0x304, 1u << 7, CsrWriteType::WRITE);
    core.writeCsr(0x300, 1u << 3, CsrWriteType::WRITE);
    core.setInterruptInputs(0, false, true);

    core.stepOneCycle(); // CSRU samples timer_irq on this edge.
    core.ifid_cur = {true, 0x40, 0x00000013, 4}; // addi x0, x0, 0
    core.stepOneCycle();

    EXPECT_EQ(core.getCSR(0x341), 0x44u);
    EXPECT_EQ(core.getPC(), 0x100u);
    EXPECT_TRUE(core.idexValid());
    EXPECT_EQ(core.idex_cur.pc, 0x40u);
}

TEST(PipelineCsrTrapTest, DataResponseAndTimerInterruptCompleteSameEdge)
{
    PipelineCore core;
    core.reset(0, 0x200);
    disableFetch(core);
    core.writeCsr(0x304, 1u << 7, CsrWriteType::WRITE);
    core.writeCsr(0x300, 1u << 3, CsrWriteType::WRITE);
    core.setInterruptInputs(0, false, true);

    constexpr uint32_t Address = 0x20010000;
    constexpr uint32_t Loaded = 0x12345678;
    core.idex_cur.valid = true;
    core.idex_cur.pc = 0x20;
    core.idex_cur.instr = 0x0000a283; // lw x5, 0(x1)
    core.idex_cur.instr_len = 4;
    core.idex_cur.kind = InstrKind::LW;
    core.idex_cur.rd = 5;
    core.idex_cur.rs1 = 1;
    core.idex_cur.rs1_val = Address;
    core.idex_cur.reg_write = true;
    core.idex_cur.mem_read = true;
    core.idex_cur.wb_sel = WbSel::MEM;
    core.ifid_cur = {true, 0x24, 0x00000013, 4};
    core.requestTimingData =
        [](uint32_t, unsigned, bool, uint32_t) { return true; };

    core.stepOneCycle(); // Issue load; CSRU samples timer IRQ.
    ASSERT_TRUE(core.lsuStalled());
    ASSERT_TRUE(core.idexValid());
    ASSERT_TRUE(core.ifidValid());

    core.acceptDataResponse(Address, Loaded, false);
    core.stepOneCycle();

    EXPECT_FALSE(core.lsuStalled());
    ASSERT_TRUE(core.exmemValid());
    EXPECT_EQ(core.exmem_cur.kind, InstrKind::LW);
    EXPECT_EQ(core.exmem_cur.mem_data, Loaded);
    ASSERT_TRUE(core.idexValid());
    EXPECT_EQ(core.idex_cur.pc, 0x24u);
    EXPECT_EQ(core.getCSR(0x341), 0x28u);
    EXPECT_EQ(core.getPC(), 0x100u);
    EXPECT_EQ(core.getFlushCount(), 1u);
}

TEST(PipelineCsrTrapTest, GeneratedDebugAndIrcuCsrsMatchResetSurface)
{
    PipelineCore core;
    core.reset(0, 0x200);

    EXPECT_EQ(core.getCSR(0x7A5), 0u);          // tcontrol
    EXPECT_EQ(core.getCSR(0x7B0), 0x40000603u); // dcsr
    EXPECT_EQ(core.getCSR(0x7B1), 0u);          // dpc
    EXPECT_EQ(core.getCSR(0xBF0), 0u);          // msleep
    EXPECT_EQ(core.getCSR(0xBE5), 0x00008000u); // meicontext.noirq
}

TEST(PipelineCsrTrapTest, MeinextChoosesLowestIrqAndReadClearsForce)
{
    PipelineCore core;
    core.reset(0, 0x200);
    core.writeCsr(0xBE0, 0x00240000u, CsrWriteType::WRITE);
    core.writeCsr(0xBE2, 0x00240000u, CsrWriteType::WRITE);

    EXPECT_EQ(core.readCsr(0xBE4), 2u << 2);
    core.applyCsrReadSideEffects(0xBE4, 0);
    EXPECT_EQ(core.readCsr(0xBE2), 0x00200000u);
    EXPECT_EQ(core.readCsr(0xBE4), 5u << 2);
}

TEST(PipelineCsrTrapTest, ExternalInterruptTracksMeicontextAcrossMret)
{
    PipelineCore core;
    core.reset(0, 0x200);
    disableFetch(core);
    core.writeCsr(0xBE0, 0x00080000u, CsrWriteType::WRITE);
    core.writeCsr(0x304, 1u << 11, CsrWriteType::WRITE);
    core.writeCsr(0x300, 1u << 3, CsrWriteType::WRITE);
    core.setInterruptInputs(1u << 3, false, false);
    core.stepOneCycle();
    core.ifid_cur = {true, 0x40, 0x00000013, 4};
    core.stepOneCycle();

    EXPECT_EQ(core.getCSR(0xBE5) & 0x00100001u, 0x00100001u);
    core.returnFromTrap();
    EXPECT_EQ(core.getCSR(0xBE5) & 0x00100001u, 0u);
}

TEST(PipelineCsrTrapTest, DebugHaltCapturesDpcAndResumeRedirects)
{
    PipelineCore core;
    core.reset(0, 0x200);
    disableFetch(core);
    core.setDebugInputs(true, false, false, 0x12345678);
    core.stepOneCycle(); // CSRU samples halt request.
    core.setDebugInputs(false, false, false, 0x12345678);
    core.ifid_cur = {true, 0x40, 0x00000013, 4};
    core.stepOneCycle();

    EXPECT_TRUE(core.debugMode());
    EXPECT_TRUE(core.haltRequested());
    EXPECT_EQ(core.getCSR(0x7B1), 0x44u);
    EXPECT_EQ((core.getCSR(0x7B0) >> 6) & 0x7u, 3u);
    EXPECT_EQ(core.getCSR(0xBFF), 0x12345678u);

    core.setDebugInstruction(0x00700293u, true); // addi x5, x0, 7
    EXPECT_TRUE(core.debugInstructionReady());
    core.stepOneCycle();
    core.setDebugInstruction(0, false);
    for (unsigned cycle = 0; cycle < 4; ++cycle) {
        core.stepOneCycle();
    }
    EXPECT_EQ(core.getReg(5), 7u);

    core.writeCsr(0x7B0, (1u << 15) | (1u << 2),
                  CsrWriteType::WRITE);
    EXPECT_EQ(core.getCSR(0x7B0) & ((1u << 15) | (1u << 2)),
              (1u << 15) | (1u << 2));

    core.setDebugInputs(false, false, true);
    core.stepOneCycle(); // CSRU samples resume request.
    core.setDebugInputs(false, false, false);
    core.stepOneCycle();
    EXPECT_FALSE(core.debugMode());
    EXPECT_FALSE(core.haltRequested());
    EXPECT_EQ(core.getPC(), 0x44u);
}

TEST(PipelineCsrTrapTest, FenceIDecodesAsRetiringNoOp)
{
    PipelineCore core;
    core.reset(0, 0x200);
    disableFetch(core);
    core.ifid_cur = {true, 0, 0x0000100f, 4};

    core.stepOneCycle();

    ASSERT_TRUE(core.idexValid());
    EXPECT_EQ(core.idex_cur.kind, InstrKind::FENCE_I);
    EXPECT_FALSE(core.idex_cur.reg_write);
}

TEST(PipelineCsrTrapTest, RtlTerminationModeKeepsFetchingPastTextEnd)
{
    PipelineCore core;
    core.reset(0, 4);
    core.configureTextEndTermination(false);
    core.fetchInstr = [](uint32_t, uint32_t &instruction) {
        instruction = 0x00000013; // addi x0, x0, 0
        return true;
    };

    for (unsigned cycle = 0; cycle < 12; ++cycle) {
        core.stepOneCycle();
    }

    EXPECT_FALSE(core.done());
    EXPECT_GT(core.getPC(), 4u);
}

} // anonymous namespace
} // namespace gem5
