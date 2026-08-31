#include "brs/sau/conv3_csr_config.hh"

#include <array>

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

constexpr uint32_t DataBase = 0x20000000;
constexpr uint64_t DataSize = 0x00400000;

Conv3CsrConfig::Payloads
validPayloads()
{
    return {
        0x2001200020010000ULL,
        0x2001292020012900ULL,
        0x0410000100200010ULL,
        0xc300000080001b11ULL,
    };
}

Conv3Config
validFields()
{
    Conv3Config fields;
    fields.inputBase = 0x20010000;
    fields.weightBase = 0x20012000;
    fields.biasBase = 0x20012900;
    fields.outputBase = 0x20012920;
    fields.inputH = 16;
    fields.inputW = 32;
    fields.batchN = 1;
    fields.inputC = 16;
    fields.outputC = 16;
    fields.abiVersion = 1;
    fields.padding = 1;
    fields.strideMinus1 = 0;
    fields.cutbit = 12;
    fields.kernelSize = 3;
    return fields;
}

TEST(Conv3CsrConfigTest, DecodesFrozenExampleAndDerivedRanges)
{
    const Conv3DecodeResult result = Conv3CsrConfig::decode(
        validPayloads(), DataBase, DataSize);

    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.error, Conv3ConfigError::None);
    EXPECT_EQ(result.config.inputBase, 0x20010000);
    EXPECT_EQ(result.config.weightBase, 0x20012000);
    EXPECT_EQ(result.config.biasBase, 0x20012900);
    EXPECT_EQ(result.config.outputBase, 0x20012920);
    EXPECT_EQ(result.config.outputH, 16);
    EXPECT_EQ(result.config.outputW, 32);
    EXPECT_EQ(result.config.inputBytes, 8192);
    EXPECT_EQ(result.config.weightBytes, 2304);
    EXPECT_EQ(result.config.biasBytes, 32);
    EXPECT_EQ(result.config.outputBytes, 8192);
    EXPECT_EQ(result.config.inputEnd, 0x20012000);
    EXPECT_EQ(result.config.weightEnd, 0x20012900);
    EXPECT_EQ(result.config.biasEnd, 0x20012920);
    EXPECT_EQ(result.config.outputEnd, 0x20014920);
}

TEST(Conv3CsrConfigTest, CommitSnapshotsFourWordsAndClearsShadowValidity)
{
    Conv3CsrConfig config(DataBase, DataSize);
    const auto payloads = validPayloads();

    EXPECT_TRUE(config.write(SauInstruction::Set1, payloads[0]).accepted);
    EXPECT_TRUE(config.write(SauInstruction::Set2, payloads[1]).accepted);
    EXPECT_TRUE(config.write(SauInstruction::Set3, payloads[2]).accepted);
    EXPECT_EQ(config.shadowValidMask(), 0x7);
    const Conv3WriteResult commit =
        config.write(SauInstruction::Set4, payloads[3]);

    ASSERT_TRUE(commit.accepted);
    ASSERT_TRUE(commit.committed);
    ASSERT_TRUE(config.busy());
    ASSERT_TRUE(config.hasActiveConfig());
    EXPECT_EQ(config.shadowValidMask(), 0);
    EXPECT_EQ(config.activeConfig()->inputBase, 0x20010000);
    EXPECT_EQ(config.activeConfig()->outputBytes, 8192);

    EXPECT_FALSE(config.write(SauInstruction::Set1, payloads[0]).accepted);
    EXPECT_EQ(config.write(SauInstruction::Set1, payloads[0]).error,
              Conv3ConfigError::Busy);
    EXPECT_TRUE(config.completeActiveOperation());
    EXPECT_FALSE(config.busy());
    EXPECT_FALSE(config.completeActiveOperation());
}

TEST(Conv3CsrConfigTest, RequiresOrderedSingleWritesAndOnlyUsesFirstFourSets)
{
    Conv3CsrConfig config(DataBase, DataSize);
    const auto payloads = validPayloads();

    EXPECT_EQ(config.write(SauInstruction::Set2, payloads[1]).error,
              Conv3ConfigError::InvalidSequence);
    EXPECT_EQ(config.write(SauInstruction::Get1Lsb, 0).error,
              Conv3ConfigError::UnsupportedOperation);
    EXPECT_EQ(config.write(SauInstruction::Set5, 0).error,
              Conv3ConfigError::UnsupportedOperation);
    EXPECT_TRUE(config.write(SauInstruction::Set1, payloads[0]).accepted);
    EXPECT_EQ(config.write(SauInstruction::Set1, payloads[0]).error,
              Conv3ConfigError::InvalidSequence);
    EXPECT_EQ(config.write(SauInstruction::Set3, payloads[2]).error,
              Conv3ConfigError::InvalidSequence);
    EXPECT_EQ(config.shadowValidMask(), 0x1);
}

TEST(Conv3CsrConfigTest, InvalidCommitDoesNotPublishOrClearShadow)
{
    Conv3CsrConfig config(DataBase, DataSize);
    const auto payloads = validPayloads();
    config.write(SauInstruction::Set1, payloads[0]);
    config.write(SauInstruction::Set2, payloads[1]);
    config.write(SauInstruction::Set3, payloads[2]);

    auto invalid = payloads[3];
    invalid &= ~(0x7ULL << 11);
    invalid |= 2ULL << 11;
    const Conv3WriteResult rejected =
        config.write(SauInstruction::Set4, invalid);

    EXPECT_FALSE(rejected.accepted);
    EXPECT_FALSE(rejected.committed);
    EXPECT_EQ(rejected.error, Conv3ConfigError::InvalidKernelSize);
    EXPECT_FALSE(config.busy());
    EXPECT_FALSE(config.hasActiveConfig());
    EXPECT_EQ(config.shadowValidMask(), 0x7);
    EXPECT_TRUE(config.write(SauInstruction::Set4, payloads[3]).committed);
}

TEST(Conv3CsrConfigTest, RejectsProtocolReservedAndControlFields)
{
    auto payloads = validPayloads();

    payloads[2] |= 1ULL << 59;
    EXPECT_EQ(Conv3CsrConfig::decode(payloads, DataBase, DataSize).error,
              Conv3ConfigError::ReservedShapeBits);

    payloads = validPayloads();
    payloads[3] |= 1ULL << 14;
    EXPECT_EQ(Conv3CsrConfig::decode(payloads, DataBase, DataSize).error,
              Conv3ConfigError::ReservedControlBits);

    payloads = validPayloads();
    payloads[3] |= 1ULL << 32;
    EXPECT_EQ(Conv3CsrConfig::decode(payloads, DataBase, DataSize).error,
              Conv3ConfigError::ReservedControlBits);

    payloads = validPayloads();
    payloads[3] &= ~(0xffULL << 56);
    EXPECT_EQ(Conv3CsrConfig::decode(payloads, DataBase, DataSize).error,
              Conv3ConfigError::InvalidMagic);

    payloads = validPayloads();
    payloads[3] &= ~0xfULL;
    EXPECT_EQ(Conv3CsrConfig::decode(payloads, DataBase, DataSize).error,
              Conv3ConfigError::InvalidAbiVersion);

    payloads = validPayloads();
    payloads[3] &= ~(1ULL << 31);
    EXPECT_EQ(Conv3CsrConfig::decode(payloads, DataBase, DataSize).error,
              Conv3ConfigError::StartNotSet);
}

TEST(Conv3CsrConfigTest, RejectsInvalidFieldsAndShapes)
{
    Conv3Config fields = validFields();

    fields.padding = 2;
    EXPECT_EQ(Conv3CsrConfig::validateFields(fields, DataBase, DataSize).error,
              Conv3ConfigError::InvalidPadding);
    fields = validFields();
    fields.strideMinus1 = 2;
    EXPECT_EQ(Conv3CsrConfig::validateFields(fields, DataBase, DataSize).error,
              Conv3ConfigError::InvalidStride);
    fields = validFields();
    fields.cutbit = 24;
    EXPECT_EQ(Conv3CsrConfig::validateFields(fields, DataBase, DataSize).error,
              Conv3ConfigError::InvalidCutbit);
    fields = validFields();
    fields.kernelSize = 2;
    EXPECT_EQ(Conv3CsrConfig::validateFields(fields, DataBase, DataSize).error,
              Conv3ConfigError::InvalidKernelSize);
    fields = validFields();
    fields.inputH = 1;
    fields.inputW = 1;
    fields.padding = 0;
    EXPECT_EQ(Conv3CsrConfig::validateFields(fields, DataBase, DataSize).error,
              Conv3ConfigError::InvalidShape);
    fields = validFields();
    fields.inputC = 64;
    EXPECT_EQ(Conv3CsrConfig::validateFields(fields, DataBase, DataSize).error,
              Conv3ConfigError::InvalidShape);
}

TEST(Conv3CsrConfigTest, RejectsAddressAlignmentRangeAndOverlapErrors)
{
    Conv3Config fields = validFields();

    fields.biasBase |= 1;
    EXPECT_EQ(Conv3CsrConfig::validateFields(fields, DataBase, DataSize).error,
              Conv3ConfigError::BiasUnaligned);

    fields = validFields();
    fields.outputBase = fields.inputBase + 1;
    EXPECT_EQ(Conv3CsrConfig::validateFields(fields, DataBase, DataSize).error,
              Conv3ConfigError::OverlappingRanges);

    fields = validFields();
    fields.inputBase = DataBase - 1;
    EXPECT_EQ(Conv3CsrConfig::validateFields(fields, DataBase, DataSize).error,
              Conv3ConfigError::AddressOutOfRange);

    fields = validFields();
    fields.outputBase = DataBase + DataSize - fields.outputBytes + 1;
    EXPECT_EQ(Conv3CsrConfig::validateFields(fields, DataBase, DataSize).error,
              Conv3ConfigError::AddressOutOfRange);

    fields = validFields();
    fields.outputBase = 0xfffffff0;
    EXPECT_EQ(Conv3CsrConfig::validateFields(
                  fields, 0, 0x100000000ULL).error,
              Conv3ConfigError::AddressOverflow);

    EXPECT_EQ(Conv3CsrConfig::decode(
                  validPayloads(), DataBase, 0).error,
              Conv3ConfigError::InvalidDataMemoryRange);
    EXPECT_EQ(Conv3CsrConfig::decode(
                  validPayloads(), DataBase, 0x100000000ULL).error,
              Conv3ConfigError::InvalidDataMemoryRange);
}

TEST(Conv3CsrConfigTest, ResetDiscardsShadowActiveAndBusyState)
{
    Conv3CsrConfig config(DataBase, DataSize);
    const auto payloads = validPayloads();
    config.write(SauInstruction::Set1, payloads[0]);
    config.write(SauInstruction::Set2, payloads[1]);
    config.write(SauInstruction::Set3, payloads[2]);
    ASSERT_TRUE(config.write(SauInstruction::Set4, payloads[3]).committed);

    config.reset();

    EXPECT_FALSE(config.busy());
    EXPECT_FALSE(config.hasActiveConfig());
    EXPECT_EQ(config.shadowValidMask(), 0);
    EXPECT_EQ(config.shadowWord(1), 0);
}

} // namespace
} // namespace brs
} // namespace gem5
