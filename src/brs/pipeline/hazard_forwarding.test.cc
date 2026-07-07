#include "brs/pipeline/forwarding_unit.hh"
#include "brs/pipeline/hazard_unit.hh"
#include "brs/pipeline/source_operands.hh"

#include <initializer_list>

#include <gtest/gtest.h>

namespace gem5
{
namespace
{

constexpr uint32_t
encodeVector(uint8_t function7, uint8_t rs2, uint8_t rs1, uint8_t rd)
{
    return (static_cast<uint32_t>(function7) << 25) |
           (static_cast<uint32_t>(rs2) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(rd) << 7) |
           brs::VeuVectorOpcode;
}

constexpr uint32_t
encodeThreeSource(
    uint8_t rs3, uint8_t function3, uint8_t rs2, uint8_t rs1, uint8_t rd)
{
    return (static_cast<uint32_t>(rs3) << 27) |
           (uint32_t{1} << 25) |
           (static_cast<uint32_t>(rs2) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(function3) << 12) |
           (static_cast<uint32_t>(rd) << 7) |
           brs::VeuThreeSourceOpcode;
}

IDEX
loadProducer(uint8_t rd)
{
    IDEX producer;
    producer.valid = true;
    producer.kind = InstrKind::LW;
    producer.rd = rd;
    producer.mem_read = true;
    producer.reg_write = true;
    producer.wb_sel = WbSel::MEM;
    return producer;
}

TEST(SourceOperandsTest, ImmediateBitsAreNotMistakenForRs2)
{
    const uint32_t addi =
        (5u << 20) | (2u << 15) | (1u << 7) | 0x13;
    const auto sources = decodeSourceOperands(addi);

    EXPECT_TRUE(sources.usesRs1);
    EXPECT_EQ(sources.rs1, 2);
    EXPECT_FALSE(sources.usesRs2);
}

TEST(SourceOperandsTest, JalrUsesOnlyRs1)
{
    const uint32_t jalr =
        (5u << 20) | (2u << 15) | (1u << 7) | 0x67;
    const auto sources = decodeSourceOperands(jalr);

    EXPECT_TRUE(sources.usesRs1);
    EXPECT_EQ(sources.rs1, 2);
    EXPECT_FALSE(sources.usesRs2);
}

TEST(SourceOperandsTest, SpiritInstructionsExposeExactSources)
{
    const auto vget =
        decodeSourceOperands((0x105u << 20) | (6u << 15) |
                             (3u << 12) | (7u << 7) | 0x0b);
    EXPECT_TRUE(vget.usesRs1);
    EXPECT_FALSE(vget.usesRs2);
    EXPECT_FALSE(vget.usesRs3);

    const auto vadd = decodeSourceOperands(encodeVector(0, 9, 8, 7));
    EXPECT_TRUE(vadd.usesRs1);
    EXPECT_TRUE(vadd.usesRs2);
    EXPECT_FALSE(vadd.usesRs3);

    const auto vmadd =
        decodeSourceOperands(encodeThreeSource(10, 1, 9, 8, 7));
    EXPECT_TRUE(vmadd.usesRs1);
    EXPECT_TRUE(vmadd.usesRs2);
    EXPECT_TRUE(vmadd.usesRs3);
    EXPECT_EQ(vmadd.rs3, 10);
}

TEST(HazardUnitTest, StallsForEveryVeuSourceAfterLoad)
{
    HazardUnit hazards;
    IFID consumer;
    consumer.valid = true;
    consumer.instr = encodeThreeSource(10, 1, 9, 8, 7);

    for (const uint8_t source : {uint8_t{8}, uint8_t{9}, uint8_t{10}}) {
        const auto decision = hazards.resolve(consumer, loadProducer(source));
        EXPECT_TRUE(decision.stall_pc);
        EXPECT_TRUE(decision.stall_ifid);
        EXPECT_TRUE(decision.bubble_idex);
    }
}

TEST(HazardUnitTest, DoesNotStallOnUnusedImmediateBits)
{
    HazardUnit hazards;
    IFID consumer;
    consumer.valid = true;
    consumer.instr =
        (5u << 20) | (2u << 15) | (1u << 7) | 0x13;

    const auto decision = hazards.resolve(consumer, loadProducer(5));
    EXPECT_FALSE(decision.stall_pc);
    EXPECT_FALSE(decision.stall_ifid);
    EXPECT_FALSE(decision.bubble_idex);
}

TEST(ForwardingUnitTest, SelectsIndependentRs1Rs2AndRs3Paths)
{
    ForwardingUnit forwarding;
    IDEX consumer;
    consumer.valid = true;
    consumer.instr = encodeThreeSource(10, 1, 9, 8, 7);
    consumer.rs1 = 8;
    consumer.rs2 = 9;
    consumer.rs3 = 10;

    EXMEM exmem;
    exmem.valid = true;
    exmem.reg_write = true;
    exmem.rd = 8;
    exmem.wb_sel = WbSel::ALU;

    MEMWB memwb;
    memwb.valid = true;
    memwb.reg_write = true;
    memwb.rd = 9;
    memwb.wb_sel = WbSel::ALU;

    auto decision = forwarding.resolve(consumer, exmem, memwb);
    EXPECT_EQ(decision.sel_a, ForwardSel::FROM_EXMEM);
    EXPECT_EQ(decision.sel_b, ForwardSel::FROM_MEMWB);
    EXPECT_EQ(decision.sel_c, ForwardSel::NONE);

    exmem.rd = 10;
    memwb = {};
    decision = forwarding.resolve(consumer, exmem, memwb);
    EXPECT_EQ(decision.sel_a, ForwardSel::NONE);
    EXPECT_EQ(decision.sel_b, ForwardSel::NONE);
    EXPECT_EQ(decision.sel_c, ForwardSel::FROM_EXMEM);
}

TEST(ForwardingUnitTest, DoesNotForwardLoadAddressFromExmem)
{
    ForwardingUnit forwarding;
    IDEX consumer;
    consumer.valid = true;
    consumer.instr = encodeVector(0, 9, 8, 7);
    consumer.rs1 = 8;
    consumer.rs2 = 9;

    EXMEM load;
    load.valid = true;
    load.reg_write = true;
    load.rd = 8;
    load.wb_sel = WbSel::MEM;

    const auto decision = forwarding.resolve(consumer, load, {});
    EXPECT_EQ(decision.sel_a, ForwardSel::NONE);
}

} // namespace
} // namespace gem5
