#include "brs/sau/sau_n_config_adapter.hh"

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

namespace gem5::brs
{
namespace
{

constexpr uint32_t DataBase = 0x20000000;
constexpr uint64_t DataSize = 0x00400000;

SauNConfigAdapter::Payloads
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

uint64_t
payload1(const Conv3Config &config)
{
    return static_cast<uint64_t>(config.inputBase) |
        (static_cast<uint64_t>(config.weightBase) << 32);
}

uint64_t
payload2(const Conv3Config &config)
{
    return static_cast<uint64_t>(config.biasBase) |
        (static_cast<uint64_t>(config.outputBase) << 32);
}

uint64_t
payload3(const Conv3Config &config)
{
    return static_cast<uint64_t>(config.inputH) |
        (static_cast<uint64_t>(config.inputW) << 16) |
        (static_cast<uint64_t>(config.batchN) << 32) |
        (static_cast<uint64_t>(config.inputC) << 48) |
        (static_cast<uint64_t>(config.outputC) << 54);
}

uint64_t
payload4(const Conv3Config &config)
{
    return static_cast<uint64_t>(config.abiVersion) |
        (static_cast<uint64_t>(config.padding) << 4) |
        (static_cast<uint64_t>(config.strideMinus1) << 5) |
        (static_cast<uint64_t>(config.cutbit) << 6) |
        (static_cast<uint64_t>(config.kernelSize) << 11) |
        (uint64_t{1} << 31) |
        (uint64_t{0xc3} << 56);
}

void
writeFirstThree(SauNConfigAdapter &adapter, const Conv3Config &config)
{
    EXPECT_TRUE(adapter.write(SauInstruction::Set1, payload1(config)).accepted);
    EXPECT_TRUE(adapter.write(SauInstruction::Set2, payload2(config)).accepted);
    EXPECT_TRUE(adapter.write(SauInstruction::Set3, payload3(config)).accepted);
}

TEST(SauNConfigAdapter, ConvertsFrozenAbiToResolvedStreamingConfig)
{
    const auto result = SauNConfigAdapter::decode(
        validPayloads(), DataBase, DataSize);

    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.error, SauNConfigError::None);
    EXPECT_EQ(result.config.pipeline.im2col.n, 1);
    EXPECT_EQ(result.config.pipeline.im2col.c, 16);
    EXPECT_EQ(result.config.pipeline.im2col.h, 16);
    EXPECT_EQ(result.config.pipeline.im2col.w, 32);
    EXPECT_EQ(result.config.pipeline.im2col.outH, 16);
    EXPECT_EQ(result.config.pipeline.im2col.outW, 32);
    EXPECT_EQ(result.config.pipeline.im2col.kernelH, 3);
    EXPECT_EQ(result.config.pipeline.im2col.kernelW, 3);
    EXPECT_EQ(result.config.pipeline.im2col.strideH, 1);
    EXPECT_EQ(result.config.pipeline.im2col.padTop, 1);
    EXPECT_EQ(result.config.pipeline.outChannels, 16);
    EXPECT_EQ(result.config.pipeline.cutbit, 12);
    EXPECT_EQ(result.config.pipeline.im2col.inputGenerator,
              "tb_act_value_v1");
    EXPECT_EQ(result.config.pipeline.weightGenerator,
              "tb_weight_value_v1");
    EXPECT_EQ(result.config.pipeline.biasGenerator,
              "tb_bias_value_v1");

    EXPECT_EQ(result.config.derived.k, 16 * 9);
    EXPECT_EQ(result.config.derived.sharedSpad.aRows, 512);
    EXPECT_EQ(result.config.derived.sharedSpad.bRows, 144);
    EXPECT_EQ(result.config.derived.sharedSpad.cRows, 2);
    EXPECT_EQ(result.config.derived.sharedSpad.dRows, 512);
    EXPECT_EQ(result.config.derived.sharedSpad.aBase, 0);
    EXPECT_EQ(result.config.derived.sharedSpad.bBase, 512);
    EXPECT_EQ(result.config.derived.sharedSpad.cBase, 656);
    EXPECT_EQ(result.config.derived.sharedSpad.dBase, 658);
    EXPECT_TRUE(result.config.pipeline.sharedSpad.configured);
    EXPECT_EQ(result.config.pipeline.sharedSpad.dBase, 658);
}

TEST(SauNConfigAdapter, CommitsOnlyAfterBothAbiAndStreamingValidation)
{
    SauNConfigAdapter adapter(DataBase, DataSize);
    const Conv3Config config = validFields();
    writeFirstThree(adapter, config);

    const SauNWriteResult commit = adapter.write(
        SauInstruction::Set4, payload4(config));
    ASSERT_TRUE(commit.accepted);
    ASSERT_TRUE(commit.committed);
    ASSERT_TRUE(adapter.busy());
    ASSERT_NE(adapter.activeConfig(), nullptr);
    EXPECT_EQ(adapter.activeConfig()->payloads, validPayloads());
    EXPECT_EQ(adapter.activeConfig()->abi.outputBytes, 8192);

    EXPECT_EQ(adapter.write(SauInstruction::Set1, payload1(config)).error,
              SauNConfigError::AbiValidation);
    EXPECT_EQ(adapter.write(SauInstruction::Set1, payload1(config)).abiError,
              Conv3ConfigError::Busy);
    EXPECT_TRUE(adapter.completeActiveOperation());
    EXPECT_FALSE(adapter.busy());
}

TEST(SauNConfigAdapter, RejectsSauNFootprintWithoutPublishingActiveConfig)
{
    Conv3Config config;
    config.inputBase = 0x00100000;
    config.weightBase = 0x00150000;
    config.biasBase = 0x00160000;
    config.outputBase = 0x00161000;
    config.inputH = 64;
    config.inputW = 64;
    config.batchN = 1;
    config.inputC = 63;
    config.outputC = 16;
    config.abiVersion = 1;
    config.padding = 0;
    config.strideMinus1 = 0;
    config.cutbit = 0;
    config.kernelSize = 3;

    const auto validated = Conv3CsrConfig::validateFields(
        config, 0, 0x100000000ULL);
    ASSERT_TRUE(validated.valid);

    SauNConfigAdapter adapter(0, 0x100000000ULL);
    writeFirstThree(adapter, validated.config);
    const SauNWriteResult rejected = adapter.write(
        SauInstruction::Set4, payload4(validated.config));

    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(rejected.error, SauNConfigError::StreamingValidation);
    EXPECT_NE(rejected.detail.find("4096"), std::string::npos);
    EXPECT_FALSE(adapter.busy());
    EXPECT_FALSE(adapter.hasActiveConfig());
    EXPECT_EQ(adapter.csrConfig().shadowValidMask(), 0x7);
}

TEST(SauNConfigAdapter, PreservesAbiFailureReasons)
{
    auto payloads = validPayloads();
    payloads[3] ^= uint64_t{1} << 56;
    const auto result = SauNConfigAdapter::decode(
        payloads, DataBase, DataSize);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error, SauNConfigError::AbiValidation);
    EXPECT_EQ(result.abiError, Conv3ConfigError::InvalidMagic);
    EXPECT_EQ(result.detail, "ABI magic must be 0xc3");
}

} // anonymous namespace
} // namespace gem5::brs
