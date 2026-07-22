#include "brs/pipeline/pipeline_core.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace
{

constexpr uint32_t
encodeAddi(uint8_t rd, uint8_t rs1, int32_t imm)
{
    return ((static_cast<uint32_t>(imm) & 0xfff) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(rd) << 7) | 0x13;
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

TEST(PipelineLsuTimingTest, IssuesInExecuteAndForwardsWithoutLoadBubble)
{
    PipelineCore core;
    core.reset(0, 4);
    core.fetchInstr = [](uint32_t, uint32_t &) { return false; };

    constexpr uint32_t Address = 0x20010000;
    constexpr uint32_t Loaded = 0x12345678;
    constexpr uint32_t Addi = encodeAddi(6, 5, 1);

    core.idex_cur.valid = true;
    core.idex_cur.pc = 0;
    core.idex_cur.instr = 0x0000a283; // lw x5, 0(x1)
    core.idex_cur.kind = InstrKind::LW;
    core.idex_cur.rd = 5;
    core.idex_cur.rs1 = 1;
    core.idex_cur.rs1_val = Address;
    core.idex_cur.reg_write = true;
    core.idex_cur.mem_read = true;
    core.idex_cur.wb_sel = WbSel::MEM;

    core.ifid_cur.valid = true;
    core.ifid_cur.pc = 4;
    core.ifid_cur.instr = Addi;
    core.ifid_cur.instr_len = 4;

    unsigned requests = 0;
    core.requestTimingData =
        [&](uint32_t address, unsigned size, bool write, uint32_t) {
            ++requests;
            EXPECT_EQ(address, Address);
            EXPECT_EQ(size, 4);
            EXPECT_FALSE(write);
            return true;
        };

    core.stepOneCycle();
    EXPECT_EQ(requests, 1);
    EXPECT_TRUE(core.lsuStalled());
    EXPECT_TRUE(core.idexValid());
    EXPECT_FALSE(core.exmemValid());

    core.stepOneCycle();
    EXPECT_EQ(requests, 1);
    EXPECT_TRUE(core.lsuStalled());
    EXPECT_TRUE(core.idexValid());

    core.acceptDataResponse(Address, Loaded, false);
    core.stepOneCycle();
    EXPECT_FALSE(core.lsuStalled());
    ASSERT_TRUE(core.exmemValid());
    EXPECT_EQ(core.exmem_cur.kind, InstrKind::LW);
    EXPECT_EQ(core.exmem_cur.mem_data, Loaded);
    ASSERT_TRUE(core.idexValid());
    EXPECT_EQ(core.idex_cur.kind, InstrKind::ADDI);

    core.stepOneCycle();
    ASSERT_TRUE(core.exmemValid());
    EXPECT_EQ(core.exmem_cur.kind, InstrKind::ADDI);
    EXPECT_EQ(core.exmem_cur.alu_result, Loaded + 1);
}

uint32_t
completeRtlLoad(InstrKind kind, uint32_t address, uint32_t rawWord)
{
    PipelineCore core;
    core.reset(0, 4);
    core.fetchInstr = [](uint32_t, uint32_t &) { return false; };
    core.idex_cur.valid = true;
    core.idex_cur.pc = 0;
    core.idex_cur.kind = kind;
    core.idex_cur.rd = 5;
    core.idex_cur.rs1 = 1;
    core.idex_cur.rs1_val = address;
    core.idex_cur.reg_write = true;
    core.idex_cur.mem_read = true;
    core.idex_cur.wb_sel = WbSel::MEM;

    core.requestTimingData =
        [&](uint32_t requested, unsigned, bool write, uint32_t) {
            EXPECT_EQ(requested, address);
            EXPECT_FALSE(write);
            return true;
        };
    core.stepOneCycle();
    core.acceptDataResponse(address, rawWord, false);
    core.stepOneCycle();
    EXPECT_TRUE(core.exmemValid());
    return core.exmem_cur.mem_data;
}

TEST(PipelineLsuTimingTest, RtlSubwordLoadsSelectWithinCurrentWord)
{
    constexpr uint32_t Base = 0x20010000;
    constexpr uint32_t Word = 0x44332211;

    EXPECT_EQ(completeRtlLoad(InstrKind::LBU, Base + 2, Word), 0x33u);
    EXPECT_EQ(completeRtlLoad(InstrKind::LH, Base + 1, Word), 0x4433u);
    EXPECT_EQ(completeRtlLoad(InstrKind::LHU, Base + 3, Word), 0x4433u);
    EXPECT_EQ(completeRtlLoad(InstrKind::LW, Base + 3, Word), Word);
}

TEST(PipelineLsuTimingTest, ResponseAndTakenBranchCompleteOnSameEdge)
{
    PipelineCore core;
    core.reset(0, 0x200);
    core.fetchInstr = [](uint32_t, uint32_t &) { return false; };

    constexpr uint32_t Address = 0x20010000;
    constexpr uint32_t Loaded = 0x89abcdef;
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
    core.ifid_cur = {true, 0x24, encodeBranch(0, 0, 0, 8), 4};

    core.requestTimingData =
        [](uint32_t, unsigned, bool, uint32_t) { return true; };
    core.stepOneCycle();
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
    EXPECT_EQ(core.idex_cur.kind, InstrKind::BEQ);
    EXPECT_EQ(core.getPC(), 0x2cu);
    EXPECT_EQ(core.getFlushCount(), 1u);
}

} // anonymous namespace
} // namespace gem5
