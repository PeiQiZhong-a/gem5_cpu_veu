#include "brs/veu/timing_veu.hh"

#include <algorithm>
#include <map>
#include <vector>

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

std::array<uint8_t, VeuVectorBytes>
makeByteVector(uint8_t value)
{
    std::array<uint8_t, VeuVectorBytes> data;
    data.fill(value);
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
                for (unsigned byte = 0; byte < VeuVectorBytes; ++byte) {
                    if (request.writeStrobe & (uint32_t{1} << byte))
                        memory[request.address][byte] = request.data[byte];
                }
                veu.completeMemoryWrite(request.transactionId);
            } else {
                veu.completeMemoryRead(request.transactionId,
                                       memory[request.address]);
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
                for (unsigned byte = 0; byte < VeuVectorBytes; ++byte) {
                    if (request.writeStrobe & (uint32_t{1} << byte))
                        memory[request.address][byte] = request.data[byte];
                }
                veu.completeMemoryWrite(request.transactionId);
            } else {
                veu.completeMemoryRead(request.transactionId,
                                       memory[request.address]);
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
    EXPECT_EQ(veu.memoryReadCount(), 2u);
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
                for (unsigned byte = 0; byte < VeuVectorBytes; ++byte) {
                    if (request.writeStrobe & (uint32_t{1} << byte))
                        memory[request.address][byte] = request.data[byte];
                }
                veu.completeMemoryWrite(request.transactionId);
            } else {
                veu.completeMemoryRead(request.transactionId,
                                       memory[request.address]);
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

TEST(TimingVeuTest, ZeroLengthStartIsCsrOnlyNoop)
{
    TimingVeu veu;
    uint64_t requests = 0;
    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &) {
            ++requests;
            return true;
        });

    stepUntilResponse(veu, vectorStart(VeuInstruction::Add, 0x100, 0x200));

    EXPECT_FALSE(veu.operationBusy());
    EXPECT_FALSE(veu.lockIsActive());
    EXPECT_EQ(veu.csrStatus() & 1u, 0u);
    EXPECT_EQ(veu.startedOperationCount(), 0u);
    EXPECT_EQ(veu.zeroLengthNoopCount(), 1u);
    EXPECT_EQ(requests, 0u);
}

TEST(TimingVeuTest, SupportsFourOutstandingAndOutOfOrderReadResponses)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.executeLatency = 2;
    config.maxOutstandingReads = 4;
    veu.configure(config);

    std::map<uint32_t, VeuVector> memory;
    memory[0x100] = makeByteVector(1);
    memory[0x120] = makeByteVector(2);
    memory[0x200] = makeByteVector(10);
    memory[0x220] = makeByteVector(20);
    std::vector<TimingVeuMemoryRequest> pendingReads;
    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            if (request.isWrite) {
                memory[request.address] = request.data;
                veu.completeMemoryWrite(request.transactionId);
            } else {
                pendingReads.push_back(request);
            }
            return true;
        });

    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x300));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 512));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));
    for (int cycle = 0; cycle < 8 && pendingReads.size() < 4; ++cycle)
        veu.clock({});

    ASSERT_EQ(pendingReads.size(), 4u);
    EXPECT_EQ(veu.currentOutstandingReadCount(), 4u);
    EXPECT_EQ(veu.maxOutstandingReadCount(), 4u);
    for (auto request = pendingReads.rbegin();
         request != pendingReads.rend(); ++request) {
        veu.completeMemoryRead(request->transactionId,
                               memory[request->address]);
    }
    stepUntilOperationComplete(veu, 200);

    EXPECT_EQ(memory[0x300][0], 11u);
    EXPECT_EQ(memory[0x320][0], 22u);
    EXPECT_EQ(veu.chunkCount(), 2u);
    EXPECT_TRUE(veu.quiescent());
}

TEST(TimingVeuTest, ZeroMaskReadsAndComputesButSkipsWrite)
{
    TimingVeu veu;
    std::map<uint32_t, VeuVector> memory;
    memory[0x100] = makeByteVector(1);
    memory[0x200] = makeByteVector(2);
    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            EXPECT_FALSE(request.isWrite);
            veu.completeMemoryRead(request.transactionId,
                                   memory[request.address]);
            return true;
        });

    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x300));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 256));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));
    stepUntilOperationComplete(veu);

    EXPECT_EQ(veu.memoryReadCount(), 2u);
    EXPECT_EQ(veu.memoryWriteCount(), 0u);
    EXPECT_EQ(veu.zeroMaskSkippedWriteCount(), 1u);
}

TEST(TimingVeuTest, ReductionAccumulatesAcrossChunksAndWritesOnlyFinalResult)
{
    TimingVeu veu;
    std::map<uint32_t, VeuVector> memory;
    memory[0x100] = makeByteVector(2);
    memory[0x120] = makeByteVector(2);
    uint64_t writes = 0;
    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            if (request.isWrite) {
                ++writes;
                memory[request.address] = request.data;
                veu.completeMemoryWrite(request.transactionId);
            } else {
                veu.completeMemoryRead(request.transactionId,
                                       memory[request.address]);
            }
            return true;
        });

    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x300));
    stepUntilResponse(veu, csrWrite(VeuCsr::Config, 0x500));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 512));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::ReduceSum, 0x100, 0));
    stepUntilOperationComplete(veu, 200);

    EXPECT_EQ(writes, 1u);
    EXPECT_EQ(lane(memory[0x300], 0), 128u);
    EXPECT_EQ(veu.memoryReadCount(), 2u);
}

TEST(TimingVeuTest, ScalarShiftAndNarrowClipMatchMeasuredModeBehavior)
{
    VeuFunctionalExecutor executor;
    VeuFunctionalInput input;
    input.config = 0xf00;
    input.scalar = 1;
    input.instruction = VeuInstruction::ShiftRightLogical;
    input.source2 = makeByteVector(0xfd);
    auto result = executor.execute(input);
    EXPECT_TRUE(std::all_of(result.data.begin(), result.data.end(),
                            [](uint8_t value) { return value == 0x7e; }));

    executor.reset();
    input = {};
    input.config = 0xe00;
    input.scalar = 0x004000c0;
    input.instruction = VeuInstruction::NarrowClip;
    input.source2 = makeByteVector(0xff);
    result = executor.execute(input);
    EXPECT_TRUE(std::all_of(result.data.begin(), result.data.end(),
                            [](uint8_t value) { return value == 0x40; }));

    executor.reset();
    input.config = 0xf00;
    result = executor.execute(input);
    EXPECT_TRUE(std::all_of(result.data.begin(), result.data.end(),
                            [](uint8_t value) { return value == 0xff; }));
}

TEST(TimingVeuTest, VectorLengthRoundsToFullChunksWithoutTailMasking)
{
    const std::array<uint32_t, 8> lengths =
        {1, 255, 256, 257, 511, 512, 1024, 2048};
    for (const uint32_t length : lengths) {
        TimingVeu veu;
        std::vector<TimingVeuMemoryRequest> requests;
        veu.setMemoryRequestCallback(
            [&](const TimingVeuMemoryRequest &request) {
                requests.push_back(request);
                if (request.isWrite) {
                    veu.completeMemoryWrite(request.transactionId);
                } else {
                    veu.completeMemoryRead(request.transactionId,
                                           makeByteVector(1));
                }
                return true;
            });
        stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x300));
        stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, length));
        stepUntilResponse(veu,
            vectorStart(VeuInstruction::Add, 0x100, 0x200));
        stepUntilOperationComplete(veu, 500);

        const uint32_t chunks = (length + 255) / 256;
        EXPECT_EQ(veu.chunkCount(), chunks) << "VLEN=" << length;
        EXPECT_EQ(veu.memoryReadCount(), chunks * 2) << "VLEN=" << length;
        EXPECT_EQ(veu.memoryWriteCount(), chunks) << "VLEN=" << length;
        for (const auto &request : requests) {
            if (request.isWrite) {
                EXPECT_EQ(request.writeStrobe, 0xffffffffu)
                    << "VLEN=" << length;
                EXPECT_EQ(request.address,
                          0x300u + request.chunkIndex * VeuVectorBytes);
            }
        }
    }
}

TEST(TimingVeuTest, RetriesRejectedRequestAndArbitratesStoresBeforeReads)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.executeLatency = 1;
    config.vsuLatency = 1;
    veu.configure(config);
    bool rejectFirst = true;
    std::map<uint32_t, VeuVector> memory;
    for (uint32_t chunk = 0; chunk < 8; ++chunk) {
        memory[0x100 + chunk * VeuVectorBytes] = makeByteVector(1);
        memory[0x200 + chunk * VeuVectorBytes] = makeByteVector(2);
    }
    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            if (rejectFirst) {
                rejectFirst = false;
                return false;
            }
            if (request.isWrite) {
                memory[request.address] = request.data;
                veu.completeMemoryWrite(request.transactionId);
            } else {
                veu.completeMemoryRead(request.transactionId,
                                       memory[request.address]);
            }
            return true;
        });
    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x300));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 2048));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));
    stepUntilOperationComplete(veu, 500);

    EXPECT_GE(veu.retryCount(), 1u);
    EXPECT_GT(veu.storePriorityCount(), 0u);
    EXPECT_EQ(veu.memoryReadCount(), 16u);
    EXPECT_EQ(veu.memoryWriteCount(), 8u);
    EXPECT_TRUE(veu.quiescent());
}

TEST(TimingVeuTest, RetriesStoreTokenWithoutReissuingIt)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.executeLatency = 1;
    config.vsuLatency = 1;
    veu.configure(config);
    bool rejectFirstWrite = true;
    std::map<uint32_t, VeuVector> memory;
    memory[0x100] = makeByteVector(0x31);
    memory[0x120] = makeByteVector(0x42);
    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            if (request.isWrite && rejectFirstWrite) {
                rejectFirstWrite = false;
                return false;
            }
            if (request.isWrite) {
                memory[request.address] = request.data;
                veu.completeMemoryWrite(request.transactionId);
            } else {
                veu.completeMemoryRead(request.transactionId,
                                       memory[request.address]);
            }
            return true;
        });
    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x300));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 512));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Move, 0x100, 0));
    stepUntilOperationComplete(veu, 200);

    EXPECT_EQ(veu.memoryReadCount(), 2u);
    EXPECT_EQ(veu.memoryWriteCount(), 2u);
    EXPECT_EQ(veu.retryCount(), 1u);
    EXPECT_EQ(memory[0x300], memory[0x100]);
    EXPECT_EQ(memory[0x320], memory[0x120]);
    EXPECT_TRUE(veu.quiescent());
}

TEST(TimingVeuTest, EnforcesConfiguredInitiationInterval)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.executeLatency = 4;
    config.executeII = 3;
    veu.configure(config);
    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            if (request.isWrite)
                veu.completeMemoryWrite(request.transactionId);
            else
                veu.completeMemoryRead(request.transactionId,
                                       makeByteVector(1));
            return true;
        });
    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x300));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 1024));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));
    stepUntilOperationComplete(veu, 500);

    EXPECT_EQ(veu.vfuAcceptedCount(), 4u);
    EXPECT_EQ(veu.vfuCompletedCount(), 4u);
    EXPECT_GT(veu.vfuIIStallCount(), 0u);
    EXPECT_GE(veu.maxVfuInFlightCount(), 2u);
}

TEST(TimingVeuTest, FunctionalExecutorCoversInitialOperationSet)
{
    struct Case
    {
        VeuInstruction instruction;
        uint8_t expected;
    };
    const std::array<Case, 10> cases = {{
        {VeuInstruction::Add, 5},
        {VeuInstruction::Sub, 0xff},
        {VeuInstruction::Min, 2},
        {VeuInstruction::Max, 3},
        {VeuInstruction::And, 2},
        {VeuInstruction::Or, 3},
        {VeuInstruction::Xor, 1},
        {VeuInstruction::Multiply, 6},
        {VeuInstruction::Move, 2},
        {VeuInstruction::ShiftRightLogical, 0},
    }};
    for (const auto &test : cases) {
        VeuFunctionalExecutor executor;
        VeuFunctionalInput input;
        input.config = 0x700;
        input.instruction = test.instruction;
        input.source1 = makeByteVector(2);
        input.source2 = makeByteVector(3);
        const auto result = executor.execute(input);
        EXPECT_EQ(result.data[0], test.expected)
            << VeuFunctionalExecutor::instructionName(test.instruction);
    }

    VeuFunctionalExecutor executor;
    VeuFunctionalInput input;
    input.config = 0xf00;
    input.scalar = 1;
    input.instruction = VeuInstruction::Add;
    input.source2 = makeByteVector(3);
    EXPECT_EQ(executor.execute(input).data[0], 4u);

    executor.reset();
    input.instruction = VeuInstruction::ShiftRightArithmetic;
    input.source2 = makeByteVector(0x80);
    EXPECT_EQ(executor.execute(input).data[0], 0xc0u);
}

} // namespace
} // namespace brs
} // namespace gem5
