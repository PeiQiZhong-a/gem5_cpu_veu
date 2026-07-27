#include "brs/pipeline/pipeline_core.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace
{

void
disableFetch(PipelineCore &core)
{
    core.fetchInstr = [](uint32_t, uint32_t &) { return false; };
}

constexpr uint32_t
encodeBranch(uint8_t funct3, uint8_t rs1, uint8_t rs2, uint32_t imm)
{
    return (((imm >> 12) & 1) << 31) |
           (((imm >> 5) & 0x3f) << 25) |
           (static_cast<uint32_t>(rs2) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(funct3) << 12) |
           (((imm >> 1) & 0xf) << 8) |
           (((imm >> 11) & 1) << 7) | 0x63;
}

TEST(PipelineTickPhaseTest, EvaluateAndClockAdvanceExactlyOneCycle)
{
    PipelineCore core;
    core.reset(0, 0x200);
    disableFetch(core);

    const uint64_t cycle = core.getCycle();
    core.evaluateOneCycle();
    EXPECT_TRUE(core.cycleHasBeenEvaluated());
    EXPECT_EQ(core.getCycle(), cycle);

    // A second combinational-system evaluation in the same tick is inert.
    core.evaluateOneCycle();
    EXPECT_EQ(core.getCycle(), cycle);

    core.clockOneCycle();
    EXPECT_FALSE(core.cycleHasBeenEvaluated());
    EXPECT_EQ(core.getCycle(), cycle + 1);

    // A stray second edge without a fresh evaluate phase is also inert.
    core.clockOneCycle();
    EXPECT_EQ(core.getCycle(), cycle + 1);
}

TEST(PipelineControlFlowTest, TakenBranchRedirectsInIdWithIeuBypass)
{
    PipelineCore core;
    core.reset(0, 0x200);
    disableFetch(core);
    core.regs[2] = 5;

    core.idex_cur.valid = true;
    core.idex_cur.pc = 0;
    core.idex_cur.instr = 0x00500093; // addi x1, x0, 5
    core.idex_cur.kind = InstrKind::ADDI;
    core.idex_cur.rd = 1;
    core.idex_cur.imm = 5;
    core.idex_cur.reg_write = true;
    core.idex_cur.wb_sel = WbSel::ALU;

    core.ifid_cur = {true, 4, encodeBranch(0, 1, 2, 8), 4};
    core.stepOneCycle();

    EXPECT_EQ(core.getPC(), 12u);
    EXPECT_EQ(core.getFlushCount(), 1u);
    ASSERT_TRUE(core.idexValid());
    EXPECT_EQ(core.idex_cur.kind, InstrKind::BEQ);

    core.stepOneCycle();
    EXPECT_EQ(core.getPC(), 12u);
    EXPECT_EQ(core.getFlushCount(), 1u);
}

TEST(PipelineControlFlowTest, JalrPreservesOddRtlTarget)
{
    PipelineCore core;
    core.reset(0, 0x200);
    disableFetch(core);
    core.regs[3] = 0x21;
    core.ifid_cur = {true, 0, 0x002182e7, 4}; // jalr x5, 2(x3)

    core.stepOneCycle();

    EXPECT_EQ(core.getPC(), 0x23u);
    EXPECT_EQ(core.getFlushCount(), 1u);
    ASSERT_TRUE(core.idexValid());
    EXPECT_EQ(core.idex_cur.kind, InstrKind::JALR);
}

TEST(PipelineControlFlowTest, JalRedirectsInIdAndDoesNotRedirectAgainInEx)
{
    PipelineCore core;
    core.reset(0, 0x200);
    disableFetch(core);
    core.ifid_cur = {true, 0, 0x008000efu, 4}; // jal x1, 8

    core.stepOneCycle();
    EXPECT_EQ(core.getPC(), 8u);
    EXPECT_EQ(core.getFlushCount(), 1u);

    core.stepOneCycle();
    EXPECT_EQ(core.getPC(), 8u);
    EXPECT_EQ(core.getFlushCount(), 1u);
    ASSERT_TRUE(core.exmemValid());
    EXPECT_EQ(core.exmem_cur.alu_result, 4u);
}

TEST(PipelineControlFlowTest, SequentialJalDoesNotFlushWithoutBtb)
{
    PipelineCore core;
    core.reset(0, 0x200);
    disableFetch(core);
    core.ifid_cur = {true, 0, 0x004000efu, 4}; // jal x1, 4

    core.stepOneCycle();

    EXPECT_EQ(core.getFlushCount(), 0u);
    ASSERT_TRUE(core.idexValid());
    EXPECT_EQ(core.idex_cur.kind, InstrKind::JAL);
}

TEST(PipelineDecodeTest, UnknownFullWidthInstructionFlowsAndRetires)
{
    PipelineCore core;
    core.reset(0, 0x200);
    disableFetch(core);
    core.ifid_cur = {true, 0, 0xffffffffu, 4};

    core.stepOneCycle();
    ASSERT_TRUE(core.idexValid());
    EXPECT_EQ(core.idex_cur.kind, InstrKind::INVALID);

    core.stepOneCycle();
    ASSERT_TRUE(core.exmemValid());
    EXPECT_EQ(core.exmem_cur.kind, InstrKind::INVALID);
    core.stepOneCycle();
    core.stepOneCycle();
    EXPECT_EQ(core.getRetiredInstCount(), 1u);
}

TEST(PipelineDecodeTest, ZeroCompressedEncodingFollowsGeneratedRtlHint)
{
    PipelineCore core;
    core.reset(0, 0x200);
    disableFetch(core);
    core.ifid_cur = {true, 0, 0x00000000u, 2};

    core.stepOneCycle();
    ASSERT_TRUE(core.idexValid());
    EXPECT_EQ(core.idex_cur.kind, InstrKind::ADDI);
    EXPECT_EQ(core.idex_cur.rd, 8);
    EXPECT_EQ(core.idex_cur.rs1, 2);
    EXPECT_EQ(core.idex_cur.imm, 0);
    EXPECT_EQ(core.getRetiredInstCount(), 0u);
}

TEST(PipelineFpPathTest, MovesBetweenIntegerAndFpRegisterBanks)
{
    PipelineCore toInteger;
    toInteger.reset(0, 0x200);
    disableFetch(toInteger);
    toInteger.fp_regs[3] = 0xdeadbeef;
    toInteger.ifid_cur = {
        true, 0, 0xe0000053u | (3u << 15) | (5u << 7), 4};

    for (unsigned i = 0; i < 4; ++i) {
        toInteger.stepOneCycle();
    }
    EXPECT_EQ(toInteger.getReg(5), 0xdeadbeefu);

    PipelineCore toFpZero;
    toFpZero.reset(0, 0x200);
    disableFetch(toFpZero);
    toFpZero.regs[4] = 0x12345678;
    toFpZero.ifid_cur = {
        true, 0, 0xf0000053u | (4u << 15), 4}; // fmv.w.x f0, x4

    for (unsigned i = 0; i < 4; ++i) {
        toFpZero.stepOneCycle();
    }
    EXPECT_EQ(toFpZero.getFpReg(0), 0x12345678u);
}

TEST(PipelineFpPathTest, FlwAndFswUseFpRegisterBank)
{
    PipelineCore core;
    core.reset(0, 0x200);
    disableFetch(core);
    core.regs[1] = 0x20010000;
    core.ifid_cur = {true, 0, 0x0000a107, 4}; // flw f2, 0(x1)
    core.stepOneCycle();

    ASSERT_TRUE(core.idexValid());
    EXPECT_EQ(core.idex_cur.kind, InstrKind::FLW);
    EXPECT_TRUE(core.idex_cur.rd_fp);

    unsigned loadRequests = 0;
    core.requestTimingData =
        [&](uint32_t address, unsigned size, bool write, uint32_t) {
            ++loadRequests;
            EXPECT_EQ(address, 0x20010000u);
            EXPECT_EQ(size, 4u);
            EXPECT_FALSE(write);
            return true;
        };
    core.stepOneCycle();
    EXPECT_EQ(loadRequests, 1u);
    core.acceptDataResponse(0x20010000, 0x89abcdef, false);
    core.stepOneCycle();
    core.stepOneCycle();
    core.stepOneCycle();
    EXPECT_EQ(core.getFpReg(2), 0x89abcdefu);

    PipelineCore store;
    store.reset(0, 0x200);
    disableFetch(store);
    store.regs[1] = 0x20010000;
    store.fp_regs[2] = 0xcafebabe;
    store.ifid_cur = {true, 0, 0x0020a027, 4}; // fsw f2, 0(x1)
    store.stepOneCycle();

    ASSERT_TRUE(store.idexValid());
    EXPECT_EQ(store.idex_cur.kind, InstrKind::FSW);
    EXPECT_TRUE(store.idex_cur.rs2_fp);
    EXPECT_EQ(store.idex_cur.rs2_val, 0xcafebabeu);

    unsigned storeRequests = 0;
    store.requestTimingData =
        [&](uint32_t address, unsigned size, bool write, uint32_t data) {
            ++storeRequests;
            EXPECT_EQ(address, 0x20010000u);
            EXPECT_EQ(size, 4u);
            EXPECT_TRUE(write);
            EXPECT_EQ(data, 0xcafebabeu);
            return true;
        };
    store.stepOneCycle();
    EXPECT_EQ(storeRequests, 1u);
}

TEST(PipelineFpPathTest, ArithmeticMatchesGeneratedRtlPermanentStall)
{
    PipelineCore core;
    core.reset(0, 0x200);
    disableFetch(core);
    core.ifid_cur = {true, 0, 0x003100d3, 4}; // fadd.s f1, f2, f3

    core.stepOneCycle();
    ASSERT_TRUE(core.idexValid());
    EXPECT_EQ(core.idex_cur.kind, InstrKind::FP_ARITH);

    core.stepOneCycle();
    EXPECT_TRUE(core.fpStalled());
    EXPECT_TRUE(core.idexValid());
    EXPECT_FALSE(core.exmemValid());

    core.stepOneCycle();
    EXPECT_TRUE(core.fpStalled());
    EXPECT_TRUE(core.idexValid());
    EXPECT_FALSE(core.exmemValid());
}

} // anonymous namespace
} // namespace gem5
