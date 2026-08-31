#include "brs/pipeline/sau_decode.hh"

#include <array>

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

TEST(SpiritSauDecodeTest, DecodesAllTwelveInstructions)
{
    for (uint8_t index = 0; index < SauSlotCount * 3; ++index) {
        const SauInstruction operation =
            static_cast<SauInstruction>(index + 1);
        const uint8_t slot = index / 3 + 1;
        const uint8_t form = index % 3;
        const uint32_t instruction =
            encodeSauInstruction(operation, 9, 10, 11);
        const SauDecodeInfo decoded =
            decodeSpiritSauInstruction(instruction);

        SCOPED_TRACE(static_cast<unsigned>(index));
        ASSERT_TRUE(decoded.valid);
        EXPECT_EQ(decoded.operation, operation);
        EXPECT_EQ(decoded.slot, slot);
        EXPECT_EQ(decoded.rd, 9);
        EXPECT_EQ(decoded.rs1, 10);
        EXPECT_EQ(decoded.rs2, 11);
        EXPECT_TRUE(decoded.usesRs1);
        EXPECT_TRUE(decoded.usesRs2);
        EXPECT_TRUE(decoded.writesRd);
        EXPECT_EQ(
            decoded.csrAddr,
            SauCsrBase + static_cast<uint16_t>((slot - 1) * 2) +
                (form == 2 ? 1 : 0));
        EXPECT_EQ(decoded.csrWrite, form == 0);
        EXPECT_EQ(decoded.csrRead, form != 0);
        EXPECT_EQ(
            decoded.writeType,
            form == 0 ? SauWriteType::Set : SauWriteType::Clear);
        EXPECT_EQ(decoded.veStart, 0);
    }
}

TEST(SpiritSauDecodeTest, DecodesSetWordsPresentInArchivedConvProgram)
{
    struct ArchiveInstruction
    {
        uint32_t pc;
        uint32_t word;
        SauInstruction operation;
        uint8_t slot;
        uint8_t rs1;
        uint8_t rs2;
    };
    // SHA-256 and archive provenance are recorded alongside the generated
    // comparison fixtures. The two Set3/Set4 pairs are the main/tail loop
    // sites in the compressed-RV32 instruction stream.
    constexpr std::array<ArchiveInstruction, 6> instructions{{
        {0x09ca, 0x00ab906b, SauInstruction::Set1, 1, 23, 10},
        {0x0a08, 0x06c5906b, SauInstruction::Set2, 2, 11, 12},
        {0x0b40, 0x0d5d106b, SauInstruction::Set3, 3, 26, 21},
        {0x0b46, 0x135b906b, SauInstruction::Set4, 4, 23, 21},
        {0x0b64, 0x0c53906b, SauInstruction::Set3, 3, 7, 5},
        {0x0b7a, 0x1272906b, SauInstruction::Set4, 4, 5, 7},
    }};

    for (const auto &expected : instructions) {
        const SauDecodeInfo decoded =
            decodeSpiritSauInstruction(expected.word);
        SCOPED_TRACE(expected.pc);
        ASSERT_TRUE(decoded.valid);
        EXPECT_EQ(decoded.operation, expected.operation);
        EXPECT_EQ(decoded.slot, expected.slot);
        EXPECT_EQ(decoded.rd, 0);
        EXPECT_EQ(decoded.rs1, expected.rs1);
        EXPECT_EQ(decoded.rs2, expected.rs2);
        EXPECT_TRUE(decoded.csrWrite);
        EXPECT_FALSE(decoded.csrRead);
        EXPECT_EQ(decoded.csrAddr, SauCsrBase + (expected.slot - 1) * 2);
    }
}

TEST(SpiritSauDecodeTest, PreservesRtlDependenciesForGetAndSet)
{
    const auto set = decodeSpiritSauInstruction(
        encodeSauInstruction(SauInstruction::Set4, 17, 3, 4));
    const auto get = decodeSpiritSauInstruction(
        encodeSauInstruction(SauInstruction::Get4Msb, 18, 5, 6));

    EXPECT_TRUE(set.usesRs1);
    EXPECT_TRUE(set.usesRs2);
    EXPECT_TRUE(set.writesRd);
    EXPECT_TRUE(get.usesRs1);
    EXPECT_TRUE(get.usesRs2);
    EXPECT_TRUE(get.writesRd);
}

TEST(SpiritSauDecodeTest, RejectsNearbyIllegalEncodings)
{
    const uint32_t firstUnsupportedFunct7 =
        (uint32_t{SauSlotCount * 3} << 25) |
        (SauFunct3 << 12) | SauOpcode;
    const uint32_t wrongFunct3 =
        encodeSauInstruction(SauInstruction::Set1) ^ (uint32_t{1} << 12);
    const uint32_t wrongOpcode =
        encodeSauInstruction(SauInstruction::Set1) ^ 0x1;

    EXPECT_FALSE(decodeSpiritSauInstruction(firstUnsupportedFunct7).valid);
    EXPECT_FALSE(decodeSpiritSauInstruction(wrongFunct3).valid);
    EXPECT_FALSE(decodeSpiritSauInstruction(wrongOpcode).valid);
    EXPECT_FALSE(decodeSpiritSauInstruction(0x00000013).valid);
}

} // namespace
} // namespace brs
} // namespace gem5
