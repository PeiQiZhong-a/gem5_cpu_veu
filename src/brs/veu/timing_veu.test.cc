#include "brs/veu/timing_veu.hh"

#include <map>

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

std::array<uint8_t, VeuVectorBytes>
makeVector(std::initializer_list<uint32_t> lanes)
{
    std::array<uint8_t, VeuVectorBytes> data = {};
    uint32_t lane = 0;
    for (uint32_t value : lanes) {
        const uint32_t offset = lane * sizeof(uint32_t);
        data[offset] = static_cast<uint8_t>(value & 0xff);
        data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
        data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xff);
        data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xff);
        ++lane;
    }
    return data;
}

uint32_t
lane(const std::array<uint8_t, VeuVectorBytes> &data, uint32_t index)
{
    const uint32_t offset = index * sizeof(uint32_t);
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

VeuRequest
csrWrite(VeuCsr csr, uint32_t value)
{
    VeuRequest request;
    request.csrAddr = static_cast<uint16_t>(csr);
    request.csrWrite = true;
    request.writeType = static_cast<uint8_t>(VeuWriteType::Write);
    request.writeData = value;
    return request;
}

VeuRequest
csrRead(VeuCsr csr)
{
    VeuRequest request;
    request.csrAddr = static_cast<uint16_t>(csr);
    request.csrRead = true;
    return request;
}

VeuRequest
vectorStart(VeuInstruction op, uint32_t raddr1, uint32_t raddr2)
{
    VeuRequest request;
    request.csrAddr = static_cast<uint16_t>(VeuCsr::ReadAddress1);
    request.csrRead = true;
    request.csrWrite = true;
    request.writeType = static_cast<uint8_t>(VeuWriteType::VectorStart);
    request.writeData = packVeuOperands(raddr1, raddr2);
    request.veStart = veuStartMask(op);
    return request;
}

void
stepUntilResponse(TimingVeu &veu, const VeuRequest &request,
                  int maxCycles = 80)
{
    for (int cycle = 0; cycle < maxCycles; ++cycle) {
        const bool valid = veu.evaluate().valid;
        veu.clock(valid ? VeuRequest{} : request);
        if (valid) {
            return;
        }
    }
    FAIL() << "TimingVeu did not respond";
}

void
stepUntilOperationComplete(TimingVeu &veu, int maxCycles = 80)
{
    for (int cycle = 0; cycle < maxCycles; ++cycle) {
        if (!veu.operationBusy()) {
            return;
        }
        veu.clock({});
    }
    FAIL() << "TimingVeu operation did not complete";
}

TEST(TimingVeuTest, CsrWriteAndReadProducesResponse)
{
    TimingVeu veu;

    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x400));
    EXPECT_EQ(veu.csrWriteAddress(), 0x400u);

    const auto read = csrRead(VeuCsr::WriteAddress);
    veu.clock(read);
    ASSERT_TRUE(veu.evaluate().valid);
    EXPECT_EQ(veu.evaluate().readData, 0x400u);
}

TEST(TimingVeuTest, VectorAddUsesTimingMemoryAndStoresResult)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.executeLatency = 1;
    veu.configure(config);

    std::map<uint32_t, std::array<uint8_t, VeuVectorBytes>> memory;
    memory[0x100] = makeVector({1, 2, 3, 4, 5, 6, 7, 8});
    memory[0x200] = makeVector({10, 20, 30, 40, 50, 60, 70, 80});

    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            if (request.isWrite) {
                memory[request.addr] = request.data;
                veu.completeMemoryWrite(request.addr);
            } else {
                veu.completeMemoryRead(request.addr, memory[request.addr]);
            }
            return true;
        });

    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x300));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, VeuVectorBits));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));

    EXPECT_TRUE(veu.operationBusy());
    EXPECT_EQ(veu.csrStatus() & 1u, 1u);
    EXPECT_EQ(veu.startedOperationCount(), 1u);
    EXPECT_EQ(veu.completedOperationCount(), 0u);

    const uint64_t responseCycle = veu.busyCycleCount();
    stepUntilOperationComplete(veu);

    EXPECT_EQ(lane(memory[0x300], 0), 11u);
    EXPECT_EQ(lane(memory[0x300], 7), 88u);
    EXPECT_EQ(veu.chunkCount(), 1u);
    EXPECT_EQ(veu.memoryReadCount(), 2u);
    EXPECT_EQ(veu.memoryWriteCount(), 1u);
    EXPECT_EQ(veu.completedOperationCount(), 1u);
    EXPECT_GT(veu.busyCycleCount(), responseCycle);
    EXPECT_EQ(veu.csrStatus() & 1u, 0u);
}

TEST(TimingVeuTest, MaskedStorePreservesDisabledBytes)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.executeLatency = 1;
    veu.configure(config);

    std::map<uint32_t, std::array<uint8_t, VeuVectorBytes>> memory;
    memory[0x100] = makeVector({1, 2, 3, 4, 5, 6, 7, 8});
    memory[0x200] = makeVector({10, 20, 30, 40, 50, 60, 70, 80});
    memory[0x300] = makeVector({0xaa, 0xbb, 0xcc, 0xdd,
                                0xee, 0xff, 0x11, 0x22});

    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            if (request.isWrite) {
                memory[request.addr] = request.data;
                veu.completeMemoryWrite(request.addr);
            } else {
                veu.completeMemoryRead(request.addr, memory[request.addr]);
            }
            return true;
        });

    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x300));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, VeuVectorBits));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0x0000000f));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));
    stepUntilOperationComplete(veu);

    EXPECT_EQ(lane(memory[0x300], 0), 11u);
    EXPECT_EQ(lane(memory[0x300], 1), 0xbbu);
    EXPECT_EQ(veu.memoryReadCount(), 3u);
    EXPECT_EQ(veu.memoryWriteCount(), 1u);
}

TEST(TimingVeuTest, ThreeSourceOperationStartsOnSecondCbuPhase)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.executeLatency = 1;
    veu.configure(config);

    std::map<uint32_t, std::array<uint8_t, VeuVectorBytes>> memory;
    memory[0x100] = makeVector({2, 2, 2, 2, 2, 2, 2, 2});
    memory[0x200] = makeVector({3, 3, 3, 3, 3, 3, 3, 3});
    memory[0x400] = makeVector({5, 5, 5, 5, 5, 5, 5, 5});

    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            if (request.isWrite) {
                memory[request.addr] = request.data;
                veu.completeMemoryWrite(request.addr);
            } else {
                veu.completeMemoryRead(request.addr, memory[request.addr]);
            }
            return true;
        });

    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x300));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, VeuVectorBits));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::MultiplyAdd, 0x100, 0x200));

    EXPECT_EQ(veu.memoryReadCount(), 0u);
    EXPECT_FALSE(veu.operationBusy());

    stepUntilResponse(veu,
        vectorStart(VeuInstruction::MultiplyAdd, 0x400, 0x400));
    EXPECT_TRUE(veu.operationBusy());
    stepUntilOperationComplete(veu);

    EXPECT_EQ(lane(memory[0x300], 0), 11u);
    EXPECT_EQ(veu.memoryReadCount(), 3u);
    EXPECT_EQ(veu.memoryWriteCount(), 1u);
}

} // namespace
} // namespace brs
} // namespace gem5
