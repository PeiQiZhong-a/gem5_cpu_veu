#include "brs/pipeline/pipeline_core.hh"

#include <cstdint>

#include <gtest/gtest.h>

namespace gem5
{
namespace
{

class TrackingSauEndpoint : public brs::SauEndpoint
{
  public:
    void reset() override
    {
        ++resetCount;
        responding = false;
        recovery = false;
        accepted = 0;
        response = {};
        memoryOutput = {};
        lastMemoryResponse = {};
        memoryClockCount = 0;
    }

    brs::SauResponse evaluate() const override
    {
        return responding ? response : brs::SauResponse{};
    }

    void clock(const brs::SauRequest &request) override
    {
        if (responding) {
            responding = false;
            recovery = true;
        } else if (recovery) {
            recovery = request.hasTransaction();
        } else if (request.hasTransaction()) {
            ++accepted;
            response = {true, 0xa5a55a5a};
            responding = true;
        }
    }

    brs::SauMemoryOutput evaluateMemory() const override
    {
        return memoryOutput;
    }

    void clockMemory(const brs::SauMemoryResponse &memoryResponse) override
    {
        lastMemoryResponse = memoryResponse;
        ++memoryClockCount;
    }

    uint32_t resetCount = 0;
    uint32_t accepted = 0;
    bool responding = false;
    bool recovery = false;
    brs::SauResponse response;
    brs::SauMemoryOutput memoryOutput;
    brs::SauMemoryResponse lastMemoryResponse;
    uint32_t memoryClockCount = 0;
};

constexpr uint32_t
encodeAddi(uint8_t rd, uint8_t rs1, uint16_t imm)
{
    return ((static_cast<uint32_t>(imm) & 0xfff) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(rd) << 7) |
           0x13u;
}

constexpr uint32_t
encodeVeuAdd(uint8_t rd, uint8_t rs1, uint8_t rs2)
{
    return (static_cast<uint32_t>(rs2) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(rd) << 7) |
           brs::makeVeuVectorInstructionMatch(0);
}

void
loadMinimalSauProgram(PipelineCore &core)
{
    core.program.clear();
    core.program.instr_mem[0] = encodeAddi(1, 0, 0x11);
    core.program.instr_mem[1] = encodeAddi(2, 0, 0x22);
    core.program.instr_mem[2] = brs::encodeSauInstruction(
        brs::SauInstruction::Set1, 0, 1, 2);
    core.program.instr_mem[3] = brs::encodeSauInstruction(
        brs::SauInstruction::Get1Lsb, 5, 0, 0);
    core.program.instr_mem[4] = brs::encodeSauInstruction(
        brs::SauInstruction::Get1Msb, 6, 0, 0);
    core.program.instr_mem[5] = encodeAddi(7, 5, 3);
    core.program.program_words = 6;
}

void
runUntilDone(PipelineCore &core, int maxCycles = 120)
{
    for (int i = 0; i < maxCycles && !core.done(); ++i) {
        core.stepOneCycle();
    }
}

TEST(PipelineSauTest, RunsSetAndGetThroughStubAndWriteback)
{
    PipelineCore core;
    core.reset(0x80000000u, 0x80000018u);
    core.configureStubSau(2);
    loadMinimalSauProgram(core);

    runUntilDone(core);

    ASSERT_TRUE(core.done());
    EXPECT_EQ(core.getSauIssueCount(), 3);
    EXPECT_EQ(core.getSauCompleteCount(), 3);
    EXPECT_EQ(core.getStubSauAcceptedRequestCount(), 3);
    EXPECT_EQ(core.getStubSauSlotValue(1), 0x0000002200000011ULL);
    EXPECT_EQ(core.getReg(5), 0x11);
    EXPECT_EQ(core.getReg(6), 0x22);
    EXPECT_EQ(core.getReg(7), 0x14);
    EXPECT_EQ(core.getRetiredInstCount(), 6);
}

TEST(PipelineSauTest, ForwardsBothSetOperandsFromOlderInstructions)
{
    PipelineCore core;
    core.reset(0x80000000u, 0x8000000cu);
    core.configureStubSau(1);
    core.program.clear();
    core.program.instr_mem[0] = encodeAddi(1, 0, 0x123);
    core.program.instr_mem[1] = encodeAddi(2, 0, 0x456);
    core.program.instr_mem[2] = brs::encodeSauInstruction(
        brs::SauInstruction::Set7, 0, 1, 2);
    core.program.program_words = 3;

    runUntilDone(core);

    ASSERT_TRUE(core.done());
    EXPECT_EQ(core.getStubSauSlotValue(7), 0x0000045600000123ULL);
    EXPECT_GE(core.getForwardCount(), 1);
}

TEST(PipelineSauTest, UnsupportedSauEncodingRetiresAsInvalid)
{
    PipelineCore core;
    core.reset(0x80000000u, 0x80000004u);
    core.program.clear();
    core.program.instr_mem[0] =
        (uint32_t{brs::SauSlotCount * 3} << 25) |
        (brs::SauFunct3 << 12) | brs::SauOpcode;
    core.program.program_words = 1;

    runUntilDone(core);

    ASSERT_TRUE(core.done());
    EXPECT_EQ(core.getSauIssueCount(), 0);
    EXPECT_EQ(core.getStubSauAcceptedRequestCount(), 0);
    EXPECT_EQ(core.getRetiredInstCount(), 1);
}

TEST(PipelineSauTest, CommonHcRouterNeverBroadcastsAcrossVeuAndSau)
{
    PipelineCore core;
    core.reset(0x80000000u, 0x80000014u);
    core.configureStubSau(1);
    core.configureFakeVeu(1, 0xc001cafe);
    core.program.clear();
    core.program.instr_mem[0] = encodeAddi(1, 0, 0x11);
    core.program.instr_mem[1] = encodeAddi(2, 0, 0x22);
    core.program.instr_mem[2] = brs::encodeSauInstruction(
        brs::SauInstruction::Set1, 0, 1, 2);
    core.program.instr_mem[3] = encodeVeuAdd(3, 1, 2);
    core.program.instr_mem[4] = brs::encodeSauInstruction(
        brs::SauInstruction::Get1Lsb, 4, 0, 0);
    core.program.program_words = 5;

    runUntilDone(core);

    ASSERT_TRUE(core.done());
    EXPECT_EQ(core.getStubSauAcceptedRequestCount(), 2);
    EXPECT_EQ(core.getFakeVeuAcceptedRequestCount(), 1);
    EXPECT_EQ(core.getSauIssueCount(), 2);
    EXPECT_EQ(core.getVeuIssueCount(), 1);
    EXPECT_EQ(core.getReg(3), 0xc001cafe);
    EXPECT_EQ(core.getReg(4), 0x11);
}

TEST(PipelineSauTest, ConfiguredReadyLatencyHasExactHandshakeCycleCount)
{
    for (uint32_t latency = 1; latency <= 4; ++latency) {
        PipelineCore core;
        core.reset(0x80000000u, 0x80000004u);
        core.configureStubSau(latency);
        core.program.clear();
        core.program.instr_mem[0] = brs::encodeSauInstruction(
            brs::SauInstruction::Set1, 0, 0, 0);
        core.program.program_words = 1;

        runUntilDone(core);

        ASSERT_TRUE(core.done()) << "latency=" << latency;
        EXPECT_EQ(core.getStubSauAcceptedRequestCount(), 1)
            << "latency=" << latency;
        // One EX cycle launches the HC request. The endpoint then responds
        // exactly `latency` cycles after sampling it.
        EXPECT_EQ(core.getSauCsrHandshakeCycles(), latency + 1)
            << "latency=" << latency;
    }
}

TEST(PipelineSauTest, AttachedSauEndpointCarriesResetHcAndSramSignals)
{
    PipelineCore core;
    core.reset(0x80000000u, 0x80000004u);
    TrackingSauEndpoint endpoint;
    core.attachSauEndpoint(endpoint);
    ASSERT_EQ(endpoint.resetCount, 1);

    endpoint.memoryOutput.request.valid = true;
    endpoint.memoryOutput.request.address = 0x29120020;
    endpoint.memoryOutput.request.writeStrobe = 0x0000000f;
    endpoint.memoryOutput.request.writeData[0] = 0x12;
    endpoint.memoryOutput.crossbarStart = true;

    const auto memoryOutput = core.evaluateSauMemory();
    EXPECT_TRUE(memoryOutput.request.valid);
    EXPECT_EQ(memoryOutput.request.address, 0x29120020);
    EXPECT_EQ(memoryOutput.request.writeStrobe, 0x0000000f);
    EXPECT_EQ(memoryOutput.request.writeData[0], 0x12);
    EXPECT_TRUE(memoryOutput.crossbarStart);

    brs::SauMemoryResponse memoryResponse;
    memoryResponse.valid = true;
    memoryResponse.readData[31] = 0x7e;
    core.clockSauMemory(memoryResponse);
    // clockSauMemory() only drives the response pins.  The endpoint samples
    // those pins atomically with HC at the next committed core edge.
    EXPECT_EQ(endpoint.memoryClockCount, 0);
    core.evaluateOneCycle();
    EXPECT_EQ(endpoint.memoryClockCount, 0);
    core.clockOneCycle();
    EXPECT_EQ(endpoint.memoryClockCount, 1);
    EXPECT_TRUE(endpoint.lastMemoryResponse.valid);
    EXPECT_EQ(endpoint.lastMemoryResponse.readData[31], 0x7e);

    // Discard the intentionally empty tick used to verify the endpoint API.
    core.reset(0x80000000u, 0x80000004u);
    core.program.clear();
    core.program.instr_mem[0] = brs::encodeSauInstruction(
        brs::SauInstruction::Get1Lsb, 9, 0, 0);
    core.program.program_words = 1;
    runUntilDone(core);

    ASSERT_TRUE(core.done());
    EXPECT_EQ(endpoint.accepted, 1);
    EXPECT_EQ(core.getReg(9), 0xa5a55a5a);
    EXPECT_EQ(core.getSauIssueCount(), 1);
    EXPECT_EQ(core.getSauCompleteCount(), 1);
}

} // namespace
} // namespace gem5
