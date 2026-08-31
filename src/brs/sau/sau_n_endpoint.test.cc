#include "brs/sau/sau_n_endpoint.hh"

#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace gem5::brs
{
namespace
{

constexpr uint32_t DataBase = 0x29120000;
constexpr uint64_t DataSize = 0x00030000;
constexpr uint32_t InputBase = 0x29130000;
constexpr uint32_t WeightBase = 0x29132000;
constexpr uint32_t BiasBase = 0x29132900;
constexpr uint32_t OutputBase = 0x29132920;

Conv3Config
simpleConfig()
{
    Conv3Config config;
    config.inputBase = InputBase;
    config.weightBase = WeightBase;
    config.biasBase = BiasBase;
    config.outputBase = OutputBase;
    config.inputH = 3;
    config.inputW = 3;
    config.batchN = 1;
    config.inputC = 1;
    config.outputC = 1;
    config.abiVersion = 1;
    config.padding = 0;
    config.strideMinus1 = 0;
    config.cutbit = 0;
    config.kernelSize = 3;
    return config;
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

SauRequest
setRequest(uint8_t slot, uint64_t payload)
{
    SauRequest request;
    request.csrAddr = SauCsrBase + static_cast<uint16_t>((slot - 1) * 2);
    request.csrWrite = true;
    request.writeType = static_cast<uint8_t>(SauWriteType::Set);
    request.writeData = payload;
    return request;
}

void
seedTensors(DutKuiMemoryModel &memory, uint8_t activation)
{
    for (uint32_t index = 0; index < 9; ++index) {
        memory.writeByte(InputBase + index, activation);
        memory.writeByte(WeightBase + index, 0xfe);
    }
    memory.writeByte(BiasBase, 0xff);
    memory.writeByte(BiasBase + 1, 0xff);
    memory.writeByte(OutputBase, 0xa5);
}

void
sendShadowWrite(SauNEndpoint &endpoint, uint8_t slot, uint64_t payload)
{
    const SauRequest request = setRequest(slot, payload);
    endpoint.clockTick(request, {});
    ASSERT_TRUE(endpoint.evaluate().valid);
    endpoint.clockTick(request, {});
    EXPECT_FALSE(endpoint.evaluate().valid);
    endpoint.clockTick({}, {});
    EXPECT_EQ(endpoint.state(), SauNEndpoint::State::Idle);
}

void
sendFirstThree(SauNEndpoint &endpoint, const Conv3Config &config)
{
    sendShadowWrite(endpoint, 1, payload1(config));
    sendShadowWrite(endpoint, 2, payload2(config));
    sendShadowWrite(endpoint, 3, payload3(config));
}

TEST(SauNEndpoint, RunsLocalModelWithOwnershipPulsesAndSharedSramWriteback)
{
    DutKuiMemoryModel memory;
    seedTensors(memory, 1);
    SauNEndpoint endpoint(memory, DataBase, DataSize);
    const Conv3Config config = simpleConfig();
    sendFirstThree(endpoint, config);

    const SauRequest heldStart = setRequest(4, payload4(config));
    endpoint.clockTick(heldStart, {});
    ASSERT_EQ(endpoint.state(), SauNEndpoint::State::Starting);

    bool sawStart = false;
    bool sawDone = false;
    for (uint64_t attempt = 0; attempt < 100000; ++attempt) {
        const SauMemoryOutput output = endpoint.evaluateMemory();
        EXPECT_FALSE(output.request.valid);
        const SauMemoryResponse oldResponse = memory.evaluate().sau;
        memory.clockEdge(false, output);

        if (output.crossbarStart) {
            ASSERT_FALSE(sawStart);
            ASSERT_FALSE(output.crossbarDone);
            sawStart = true;
            endpoint.clockTick(heldStart, oldResponse);
            continue;
        }
        if (output.crossbarDone) {
            ASSERT_TRUE(sawStart);
            ASSERT_FALSE(endpoint.evaluate().valid);
            sawDone = true;
            endpoint.clockTick(heldStart, oldResponse);
            EXPECT_TRUE(endpoint.evaluate().valid);
            break;
        }

        ASSERT_EQ(endpoint.state(), SauNEndpoint::State::Running);
        endpoint.clockTick(heldStart, oldResponse);
    }

    EXPECT_TRUE(sawStart);
    EXPECT_TRUE(sawDone);
    EXPECT_EQ(memory.crossbarState(), DutKuiDataCrossbar::State::Idle);
    EXPECT_EQ(endpoint.operationStartCount(), 1u);
    EXPECT_EQ(endpoint.operationCompleteCount(), 1u);
    EXPECT_GT(endpoint.modelTickCount(), 0u);
    ASSERT_NE(endpoint.lastModelCycle(), nullptr);
    EXPECT_TRUE(endpoint.lastModelCycle()->drained);
    ASSERT_NE(endpoint.streamingStats(), nullptr);
    EXPECT_GT(endpoint.streamingStats()->spadReadGrantsA, 0u);
    EXPECT_GT(endpoint.streamingStats()->spadReadGrantsB, 0u);
    EXPECT_GT(endpoint.streamingStats()->spadReadGrantsC, 0u);
    EXPECT_GT(endpoint.streamingStats()->spadWriteGrantsD, 0u);
    EXPECT_EQ(memory.readByte(OutputBase), uint8_t{0xed});

    endpoint.clockTick(heldStart, {});
    EXPECT_EQ(endpoint.state(), SauNEndpoint::State::Recovery);
    endpoint.clockTick({}, {});
    EXPECT_EQ(endpoint.state(), SauNEndpoint::State::Idle);
}

TEST(SauNEndpoint, ResetDestroysActiveModelWithoutClearingSharedSram)
{
    DutKuiMemoryModel memory;
    seedTensors(memory, 1);
    SauNEndpoint endpoint(memory, DataBase, DataSize);
    const Conv3Config config = simpleConfig();
    sendFirstThree(endpoint, config);
    endpoint.clockTick(setRequest(4, payload4(config)), {});
    ASSERT_EQ(endpoint.state(), SauNEndpoint::State::Starting);
    EXPECT_EQ(memory.readByte(OutputBase), uint8_t{0xa5});

    endpoint.reset();

    EXPECT_EQ(endpoint.state(), SauNEndpoint::State::Idle);
    EXPECT_FALSE(endpoint.evaluate().valid);
    EXPECT_FALSE(endpoint.evaluateMemory().crossbarStart);
    EXPECT_FALSE(endpoint.evaluateMemory().crossbarDone);
    EXPECT_EQ(memory.readByte(OutputBase), uint8_t{0xa5});
}

TEST(SauNEndpoint, RejectsExternalSramResponsesInLocalBackingMode)
{
    DutKuiMemoryModel memory;
    SauNEndpoint endpoint(memory, DataBase, DataSize);
    SauMemoryResponse response;
    response.valid = true;

    EXPECT_THROW(endpoint.clockTick({}, response), std::logic_error);
}

} // anonymous namespace
} // namespace gem5::brs
