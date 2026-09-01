#include "brs/pipeline/frontend_fetch_unit.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace
{

uint32_t
word(uint16_t high, uint16_t low)
{
    return (static_cast<uint32_t>(high) << 16) | low;
}

void
advanceResetEnd(FrontendFetchUnit &frontend, uint32_t textEnd)
{
    FrontendFetchUnit::Input input;
    input.textEnd = textEnd;
    frontend.step(input);
    frontend.step(input);
}

FrontendAligner::Input
alignerInput(bool valid, uint32_t bits, uint32_t addr)
{
    FrontendAligner::Input input;
    input.fetchInstrValid = valid;
    input.fetchInstrBits = bits;
    input.fetchInstrAddr = addr;
    return input;
}

FrontendFetchUnit::Input
responseInput(uint32_t textEnd, const FetchBlock &block, bool stall = false)
{
    FrontendFetchUnit::Input input;
    input.stall = stall;
    input.textEnd = textEnd;
    input.responseValid = true;
    input.response = block;
    return input;
}

TEST(FrontendAlignerTest, EmitsAlignedRv32Instruction)
{
    FrontendAligner aligner;
    aligner.reset(0);

    const auto out = aligner.step(alignerInput(true, 0x00500093, 0));

    EXPECT_TRUE(out.alignedInstrValid);
    EXPECT_EQ(out.alignedInstrBits, 0x00500093);
    EXPECT_EQ(out.nextInstrAddr, 4);
    EXPECT_FALSE(out.stopFetch);
}

TEST(FrontendAlignerTest, EmitsTwoCompressedInstructionsFromOneWord)
{
    FrontendAligner aligner;
    aligner.reset(0);

    const auto first = aligner.step(
        alignerInput(true, word(0x0001, 0x0001), 0));

    EXPECT_TRUE(first.alignedInstrValid);
    EXPECT_EQ(first.alignedInstrBits, 0x0001);
    EXPECT_EQ(first.nextInstrAddr, 2);
    EXPECT_TRUE(first.stopFetch);

    const auto second = aligner.step(alignerInput(false, 0, 2));

    EXPECT_TRUE(second.alignedInstrValid);
    EXPECT_EQ(second.alignedInstrBits, 0x0001);
    EXPECT_EQ(second.nextInstrAddr, 4);
    EXPECT_FALSE(second.stopFetch);
}

TEST(FrontendAlignerTest, ConcatenatesRv32InstructionAcrossWords)
{
    FrontendAligner aligner;
    aligner.reset(0);

    aligner.step(alignerInput(true, word(0x567b, 0x0001), 0));

    const auto out = aligner.step(
        alignerInput(true, word(0x0001, 0x1234), 2));

    EXPECT_TRUE(out.alignedInstrValid);
    EXPECT_EQ(out.alignedInstrBits, 0x1234567b);
    EXPECT_EQ(out.nextInstrAddr, 6);
    EXPECT_TRUE(out.stopFetch);
}

TEST(FetchFifoTest, PushesOnlyValidWordsForMisalignedFetch)
{
    FetchFifo fifo(12);
    FetchBlock block;
    block.fetchAddr = 8;
    block.blockAddr = 8;
    block.words = {0, 1, 2, 3};

    EXPECT_TRUE(fifo.pushBlock(block));
    ASSERT_EQ(fifo.count(), 2);
    EXPECT_EQ(fifo.front().addr, 16);
    EXPECT_EQ(fifo.front().word, 2);
    fifo.pop();
    EXPECT_EQ(fifo.front().addr, 20);
    EXPECT_EQ(fifo.front().word, 3);
}

TEST(FetchFifoTest, UpdatesCountForSameCyclePushAndPop)
{
    FetchFifo fifo(12);
    FetchFifo::CycleInput first;
    first.pushCount = 4;
    first.pushEntries = {{
        {0, 10}, {4, 11}, {8, 12}, {12, 13}
    }};
    fifo.step(first);
    ASSERT_EQ(fifo.count(), 4);

    FetchFifo::CycleInput second;
    second.pop = true;
    second.pushCount = 4;
    second.pushEntries = {{
        {16, 14}, {20, 15}, {24, 16}, {28, 17}
    }};
    fifo.step(second);

    EXPECT_EQ(fifo.count(), 7);
    EXPECT_EQ(fifo.front().addr, 4);
    EXPECT_EQ(fifo.front().word, 11);
}

TEST(FetchFifoTest, FlushAllClearsPointersAndCount)
{
    FetchFifo fifo(12);
    FetchFifo::CycleInput push;
    push.pushCount = 2;
    push.pushEntries = {{
        {0, 10}, {4, 11}, {}, {}
    }};
    fifo.step(push);
    ASSERT_EQ(fifo.count(), 2);

    FetchFifo::CycleInput flush;
    flush.flushAll = true;
    fifo.step(flush);

    EXPECT_TRUE(fifo.empty());
    EXPECT_EQ(fifo.count(), 0);

    fifo.step(push);
    EXPECT_EQ(fifo.front().addr, 0);
    EXPECT_EQ(fifo.front().word, 10);
}

TEST(FetchBusUnitTest, DiscardsFlushedInFlightResponse)
{
    FetchBusUnit ibu;
    ibu.reset(0);
    EXPECT_EQ(ibu.requestBlockAddr(), 0);
    ibu.markRequestIssued();
    ibu.flush(0x40);

    FetchBlock oldBlock;
    oldBlock.blockAddr = 0;
    oldBlock.fetchAddr = 0;
    EXPECT_FALSE(ibu.acceptResponse(oldBlock));
    EXPECT_EQ(ibu.requestBlockAddr(), 0x40);
}

TEST(FetchBusUnitTest, TracksExactRequestAcrossAlignedResponseBase)
{
    FetchBusUnit ibu;
    ibu.reset(6);

    EXPECT_EQ(ibu.requestFetchAddr(), 6);
    EXPECT_EQ(ibu.requestBlockAddr(), 4);
    ibu.markRequestIssued();

    FetchBlock block;
    block.fetchAddr = 6;
    block.blockAddr = 0;
    EXPECT_TRUE(ibu.acceptResponse(block));
    EXPECT_EQ(ibu.requestFetchAddr(), 16);
}

TEST(FrontendFetchUnitTest, WaitsTwoActiveCyclesAfterReset)
{
    FrontendFetchUnit frontend;
    frontend.reset(0);

    FrontendFetchUnit::Input input;
    input.textEnd = 16;

    EXPECT_FALSE(frontend.step(input).requestValid);
    EXPECT_FALSE(frontend.step(input).requestValid);

    const auto out = frontend.step(input);
    EXPECT_TRUE(out.requestValid);
    EXPECT_EQ(out.requestAddr, 0);
    EXPECT_EQ(out.requestFetchAddr, 0);
    EXPECT_EQ(frontend.getIbusReqCount(), 0);
}

TEST(FrontendFetchUnitTest, DoesNotEmitPastTextEnd)
{
    FrontendFetchUnit frontend;
    frontend.reset(0);
    advanceResetEnd(frontend, 16);

    FrontendFetchUnit::Input requestInput;
    requestInput.textEnd = 16;
    auto out = frontend.step(requestInput);
    ASSERT_TRUE(out.requestValid);
    frontend.markRequestIssued();

    FetchBlock block;
    block.fetchAddr = 0;
    block.blockAddr = 0;
    block.words = {0x00500093, 0x00000013, 0x00000013, 0};

    out = frontend.step(responseInput(0, block));

    EXPECT_FALSE(out.instValid);
    EXPECT_EQ(frontend.getPC(), 0);
}

TEST(FrontendFetchUnitTest, BypassesReturnedBlockWhenFifoIsEmpty)
{
    FrontendFetchUnit frontend;
    frontend.reset(0);
    advanceResetEnd(frontend, 16);

    FrontendFetchUnit::Input input;
    input.textEnd = 16;
    auto out = frontend.step(input);
    ASSERT_TRUE(out.requestValid);
    frontend.markRequestIssued();

    FetchBlock block;
    block.fetchAddr = 0;
    block.blockAddr = 0;
    block.words = {0x00500093, 0x00600113, 0x00700193, 0};

    out = frontend.step(responseInput(16, block));

    EXPECT_FALSE(out.instValid);

    input = {};
    input.textEnd = 16;
    out = frontend.step(input);
    ASSERT_TRUE(out.instValid);
    EXPECT_EQ(out.pc, 0);
    EXPECT_EQ(out.instr, 0x00500093);
    EXPECT_EQ(frontend.getPC(), 8);

    input = {};
    input.textEnd = 16;
    out = frontend.step(input);
    ASSERT_TRUE(out.instValid);
    EXPECT_EQ(out.pc, 4);
    EXPECT_EQ(out.instr, 0x00600113);
}

TEST(FrontendFetchUnitTest, BypassesFirstValidWordForMisalignedFetch)
{
    FrontendFetchUnit frontend;
    frontend.reset(8);
    advanceResetEnd(frontend, 16);

    FrontendFetchUnit::Input input;
    input.textEnd = 16;
    auto out = frontend.step(input);
    ASSERT_TRUE(out.requestValid);
    EXPECT_EQ(out.requestAddr, 8);
    frontend.markRequestIssued();

    FetchBlock block;
    block.fetchAddr = 8;
    block.blockAddr = 8;
    block.words = {0x00100093, 0x00200113, 0x00300193, 0x00400213};

    out = frontend.step(responseInput(16, block));

    EXPECT_FALSE(out.instValid);

    input = {};
    input.textEnd = 16;
    out = frontend.step(input);
    ASSERT_TRUE(out.instValid);
    EXPECT_EQ(out.pc, 8);
    EXPECT_EQ(out.instr, 0x00300193);

    input = {};
    input.textEnd = 16;
    out = frontend.step(input);
    ASSERT_TRUE(out.instValid);
    EXPECT_EQ(out.pc, 12);
    EXPECT_EQ(out.instr, 0x00400213);
}

TEST(FrontendFetchUnitTest, DoesNotReplayCrossWordInstructionBeforeNextFetch)
{
    FrontendFetchUnit frontend;
    frontend.reset(0x19aa);
    advanceResetEnd(frontend, 0x19c0);

    FrontendFetchUnit::Input input;
    input.textEnd = 0x19c0;
    auto out = frontend.step(input);
    ASSERT_TRUE(out.requestValid);
    EXPECT_EQ(out.requestFetchAddr, 0x19aa);
    frontend.markRequestIssued();

    // This is the archive's 16-byte line at 0x19a0.  The lbu at 0x19aa
    // occupies the high half of 0x19a8 and the low half of 0x19ac;
    // the following bnez at 0x19ae crosses into the next line.
    FetchBlock first;
    first.fetchAddr = 0x19aa;
    first.blockAddr = 0x19a8;
    first.words = {0x41aa0733, 0x8b569bba, 0xc5038dde, 0x1ee30004};

    out = frontend.step(responseInput(0x19c0, first));
    EXPECT_FALSE(out.instValid);

    input = {};
    input.textEnd = 0x19c0;
    out = frontend.step(input);
    EXPECT_FALSE(out.instValid);
    ASSERT_TRUE(out.requestValid);
    EXPECT_EQ(out.requestFetchAddr, 0x19b0);
    frontend.markRequestIssued();

    // The aligner has the first half of the lbu but no FIFO word for the
    // moment.  It must not replay that value as a second instruction.
    out = frontend.step(input);
    ASSERT_TRUE(out.instValid);
    EXPECT_EQ(out.pc, 0x19aa);
    EXPECT_EQ(out.instr, 0x0004c503);

    FetchBlock second;
    second.fetchAddr = 0x19b0;
    second.blockAddr = 0x19b0;
    second.words = {0x866ece05, 0x013de463, 0xfff98613, 0x86ce45c2};

    out = frontend.step(responseInput(0x19c0, second));
    EXPECT_FALSE(out.instValid);

    input = {};
    input.textEnd = 0x19c0;
    out = frontend.step(input);
    ASSERT_TRUE(out.instValid);
    EXPECT_EQ(out.pc, 0x19ae);
    EXPECT_EQ(out.instr, 0xce051ee3);
}

TEST(FrontendFetchUnitTest, AcceptsResponseWhileStalled)
{
    FrontendFetchUnit frontend;
    frontend.reset(0);
    advanceResetEnd(frontend, 16);

    FrontendFetchUnit::Input input;
    input.textEnd = 16;
    auto out = frontend.step(input);
    ASSERT_TRUE(out.requestValid);
    frontend.markRequestIssued();

    FetchBlock block;
    block.fetchAddr = 0;
    block.blockAddr = 0;
    block.words = {0x00500093, 0x00600113, 0, 0};

    out = frontend.step(responseInput(16, block, true));
    EXPECT_FALSE(out.instValid);

    input = {};
    input.textEnd = 16;
    out = frontend.step(input);
    EXPECT_FALSE(out.instValid);

    out = frontend.step(input);
    ASSERT_TRUE(out.instValid);
    EXPECT_EQ(out.pc, 0);
    EXPECT_EQ(out.instr, 0x00500093);
}

TEST(FrontendFetchUnitTest, HoldsIfOutputRegisterWhileStalled)
{
    FrontendFetchUnit frontend;
    frontend.reset(0);
    advanceResetEnd(frontend, 16);

    FrontendFetchUnit::Input input;
    input.textEnd = 16;
    auto out = frontend.step(input);
    ASSERT_TRUE(out.requestValid);
    frontend.markRequestIssued();

    FetchBlock block;
    block.fetchAddr = 0;
    block.blockAddr = 0;
    block.words = {0x00500093, 0x00600113, 0, 0};

    out = frontend.step(responseInput(16, block));
    EXPECT_FALSE(out.instValid);

    input = {};
    input.textEnd = 16;
    out = frontend.step(input);
    ASSERT_TRUE(out.instValid);
    EXPECT_EQ(out.instr, 0x00500093);

    input.stall = true;
    out = frontend.step(input);
    ASSERT_TRUE(out.instValid);
    EXPECT_EQ(out.instr, 0x00600113);

    out = frontend.step(input);
    ASSERT_TRUE(out.instValid);
    EXPECT_EQ(out.instr, 0x00600113);
}

TEST(FrontendFetchUnitTest, RequestsTrueFetchAddrForMisalignedResetPc)
{
    FrontendFetchUnit frontend;
    frontend.reset(6);
    advanceResetEnd(frontend, 32);

    FrontendFetchUnit::Input input;
    input.textEnd = 32;
    auto out = frontend.step(input);
    ASSERT_TRUE(out.requestValid);
    EXPECT_EQ(out.requestAddr, 6);
    EXPECT_EQ(out.requestFetchAddr, 6);
}

TEST(FrontendFetchUnitTest, RequestsTrueFetchAddrAfterMisalignedRedirect)
{
    FrontendFetchUnit frontend;
    frontend.reset(0);
    advanceResetEnd(frontend, 32);

    FrontendFetchUnit::Input input;
    input.textEnd = 32;
    auto out = frontend.step(input);
    ASSERT_TRUE(out.requestValid);
    frontend.markRequestIssued();

    input = {};
    input.textEnd = 32;
    input.redirect = true;
    input.redirectTarget = 8;
    out = frontend.step(input);

    EXPECT_FALSE(out.requestValid);
    EXPECT_EQ(frontend.getPC(), 8);

    FetchBlock oldBlock;
    oldBlock.fetchAddr = 0;
    oldBlock.blockAddr = 0;
    oldBlock.words = {0, 0, 0, 0};

    input = {};
    input.textEnd = 32;
    input.responseValid = true;
    input.response = oldBlock;
    out = frontend.step(input);

    EXPECT_FALSE(out.requestValid);

    input = {};
    input.textEnd = 32;
    out = frontend.step(input);
    ASSERT_TRUE(out.requestValid);
    EXPECT_EQ(out.requestAddr, 8);
    EXPECT_EQ(out.requestFetchAddr, 8);
}

TEST(FrontendFetchUnitTest, RedirectAndOldResponseDoNotReissueSameEdge)
{
    FrontendFetchUnit frontend;
    frontend.reset(0);
    advanceResetEnd(frontend, 64);

    FrontendFetchUnit::Input input;
    input.textEnd = 64;
    auto out = frontend.step(input);
    ASSERT_TRUE(out.requestValid);
    frontend.markRequestIssued();

    FetchBlock oldBlock;
    oldBlock.fetchAddr = 0;
    oldBlock.blockAddr = 0;
    oldBlock.words = {0x00500093, 0, 0, 0};

    input = {};
    input.textEnd = 64;
    input.redirect = true;
    input.redirectTarget = 0x20;
    input.responseValid = true;
    input.response = oldBlock;
    out = frontend.step(input);

    EXPECT_FALSE(out.requestValid);
    EXPECT_FALSE(out.instValid);
    EXPECT_EQ(frontend.getPC(), 0x20);

    input = {};
    input.textEnd = 64;
    out = frontend.step(input);
    ASSERT_TRUE(out.requestValid);
    EXPECT_EQ(out.requestAddr, 0x20);
    EXPECT_EQ(out.requestFetchAddr, 0x20);
}

TEST(FrontendFetchUnitTest, RedirectClearsRegisteredIfOutput)
{
    FrontendFetchUnit frontend;
    frontend.reset(0);
    advanceResetEnd(frontend, 128);

    FrontendFetchUnit::Input input;
    input.textEnd = 128;
    auto out = frontend.step(input);
    ASSERT_TRUE(out.requestValid);
    frontend.markRequestIssued();

    FetchBlock block;
    block.fetchAddr = 0;
    block.blockAddr = 0;
    block.words = {0x00500093, 0x00600113, 0, 0};

    out = frontend.step(responseInput(128, block));
    EXPECT_FALSE(out.instValid);

    input = {};
    input.textEnd = 128;
    out = frontend.step(input);
    ASSERT_TRUE(out.instValid);
    EXPECT_EQ(out.instr, 0x00500093);

    input = {};
    input.redirect = true;
    input.redirectTarget = 0x40;
    input.textEnd = 128;
    out = frontend.step(input);

    EXPECT_FALSE(out.instValid);
    EXPECT_EQ(frontend.getPC(), 0x40);
}

TEST(FrontendFetchUnitTest, IdleRedirectRequestsOnFollowingEdge)
{
    FrontendFetchUnit frontend;
    frontend.reset(0);
    advanceResetEnd(frontend, 128);

    FrontendFetchUnit::Input input;
    input.redirect = true;
    input.redirectTarget = 0x40;
    input.textEnd = 128;
    auto out = frontend.step(input);
    EXPECT_FALSE(out.requestValid);
    EXPECT_EQ(frontend.getPC(), 0x40);

    input = {};
    input.textEnd = 128;
    out = frontend.step(input);
    ASSERT_TRUE(out.requestValid);
    EXPECT_EQ(out.requestAddr, 0x40u);
}

} // anonymous namespace
} // namespace gem5
