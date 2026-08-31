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
        if (lane == VeuLaneCount) break;
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
    memory[0x100] = makeVector({1, 2, 3, 4});
    memory[0x200] = makeVector({10, 20, 30, 40});

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
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, VeuFullWriteMask));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));

    EXPECT_TRUE(veu.operationBusy());
    EXPECT_EQ(veu.csrStatus() & 1u, 1u);
    EXPECT_EQ(veu.startedOperationCount(), 1u);
    EXPECT_EQ(veu.completedOperationCount(), 0u);

    const uint64_t responseCycle = veu.busyCycleCount();
    stepUntilOperationComplete(veu);

    EXPECT_EQ(lane(memory[0x300], 0), 11u);
    EXPECT_EQ(lane(memory[0x300], 3), 44u);
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
    memory[0x100] = makeVector({1, 2, 3, 4});
    memory[0x200] = makeVector({10, 20, 30, 40});
    memory[0x300] = makeVector({0xaa, 0xbb, 0xcc, 0xdd});

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

TEST(TimingVeuTest, MikuiRejectsYinglongThreeSourceOperations)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.executeLatency = 1;
    veu.configure(config);

    std::map<uint32_t, std::array<uint8_t, VeuVectorBytes>> memory;
    memory[0x100] = makeVector({2, 2, 2, 2});
    memory[0x200] = makeVector({3, 3, 3, 3});
    memory[0x400] = makeVector({5, 5, 5, 5});

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
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, VeuFullWriteMask));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::MultiplyAdd, 0x100, 0x200));

    EXPECT_EQ(veu.memoryReadCount(), 0u);
    EXPECT_FALSE(veu.operationBusy());

    stepUntilResponse(veu,
        vectorStart(VeuInstruction::MultiplyAdd, 0x400, 0x400));
    EXPECT_FALSE(veu.operationBusy());
    EXPECT_EQ(veu.illegalOperationCount(), 1u);
    EXPECT_EQ(veu.memoryReadCount(), 0u);
    EXPECT_EQ(veu.memoryWriteCount(), 0u);
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
    memory[0x110] = makeByteVector(2);
    memory[0x200] = makeByteVector(10);
    memory[0x210] = makeByteVector(20);
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
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 256));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, VeuFullWriteMask));
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
    EXPECT_EQ(memory[0x310][0], 22u);
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
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 128));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));
    stepUntilOperationComplete(veu);

    EXPECT_EQ(veu.memoryReadCount(), 2u);
    EXPECT_EQ(veu.memoryWriteCount(), 0u);
    EXPECT_EQ(veu.zeroMaskSkippedWriteCount(), 1u);
}

TEST(TimingVeuTest, ReductionWritesMikuiRunningAndFinalResults)
{
    TimingVeu veu;
    std::map<uint32_t, VeuVector> memory;
    memory[0x100] = makeByteVector(2);
    memory[0x110] = makeByteVector(2);
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
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 256));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, VeuFullWriteMask));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::ReduceSum, 0x100, 0));
    stepUntilOperationComplete(veu, 200);

    EXPECT_EQ(writes, 2u);
    EXPECT_EQ(lane(memory[0x300], 0), 64u);
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
        {1, 127, 128, 129, 255, 256, 512, 1024};
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
        stepUntilResponse(veu, csrWrite(VeuCsr::Mask, VeuFullWriteMask));
        stepUntilResponse(veu,
            vectorStart(VeuInstruction::Add, 0x100, 0x200));
        stepUntilOperationComplete(veu, 500);

        const uint32_t chunks = (length + VeuVectorBits - 1) / VeuVectorBits;
        EXPECT_EQ(veu.chunkCount(), chunks) << "VLEN=" << length;
        EXPECT_EQ(veu.memoryReadCount(), chunks * 2) << "VLEN=" << length;
        EXPECT_EQ(veu.memoryWriteCount(), chunks) << "VLEN=" << length;
        for (const auto &request : requests) {
            if (request.isWrite) {
                EXPECT_EQ(request.writeStrobe, VeuFullWriteMask)
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
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 1024));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, VeuFullWriteMask));
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
    memory[0x110] = makeByteVector(0x42);
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
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 256));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, VeuFullWriteMask));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Move, 0x100, 0));
    stepUntilOperationComplete(veu, 200);

    EXPECT_EQ(veu.memoryReadCount(), 2u);
    EXPECT_EQ(veu.memoryWriteCount(), 2u);
    EXPECT_EQ(veu.retryCount(), 1u);
    EXPECT_EQ(memory[0x300], memory[0x100]);
    EXPECT_EQ(memory[0x310], memory[0x110]);
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
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 512));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, VeuFullWriteMask));
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

TEST(TimingVeuTest, MikuiOperationSupportMatchesVcuAndVfu)
{
    const auto slide = VeuFunctionalExecutor::describe(
        VeuInstruction::SlideUp, true);
    EXPECT_TRUE(slide.supported);
    EXPECT_TRUE(VeuFunctionalExecutor::sourceRequired(
        slide.sourceMask, VeuSource::Source2));

    const auto widen = VeuFunctionalExecutor::describe(
        VeuInstruction::WidenReduceSum, false);
    EXPECT_TRUE(widen.supported);
    EXPECT_TRUE(widen.rtlIllegal);

    const auto multiplyHigh = VeuFunctionalExecutor::describe(
        VeuInstruction::MultiplyHigh, false);
    EXPECT_TRUE(multiplyHigh.supported);
    EXPECT_TRUE(multiplyHigh.rtlIllegal);

    for (const auto instruction : {
             VeuInstruction::MultiplyAdd,
             VeuInstruction::MultiplySubtract,
             VeuInstruction::MultiplyHighSignedUnsigned}) {
        const auto info = VeuFunctionalExecutor::describe(instruction, false);
        EXPECT_FALSE(info.supported);
        EXPECT_TRUE(info.rtlIllegal);
    }
    EXPECT_FALSE(VeuFunctionalExecutor::describe(
        VeuInstruction::Compress, false).supported);
}

TEST(TimingVeuTest, SlideUsesMikui128BitChunkBoundaries)
{
    VeuVector first = {};
    VeuVector second = {};
    for (uint32_t byte = 0; byte < VeuVectorBytes; ++byte) {
        first[byte] = byte;
        second[byte] = 0x40 + byte;
    }

    VeuFunctionalExecutor executor;
    VeuFunctionalInput input;
    input.instruction = VeuInstruction::SlideUp;
    input.config = 0x800;
    input.scalar = 4;
    input.chunkCount = 2;
    input.source2 = first;
    auto result = executor.execute(input);
    EXPECT_EQ(result.data[0], 0);
    EXPECT_EQ(result.data[4], first[0]);
    input.chunkIndex = 1;
    input.source2 = second;
    result = executor.execute(input);
    EXPECT_EQ(result.data[0], first[12]);
    EXPECT_EQ(result.data[4], second[0]);

    executor.reset();
    input.instruction = VeuInstruction::SlideDown;
    input.chunkIndex = 0;
    input.source2 = first;
    result = executor.execute(input);
    EXPECT_FALSE(result.writeResult);
    input.chunkIndex = 1;
    input.source2 = second;
    result = executor.execute(input);
    EXPECT_TRUE(result.writeResult);
    EXPECT_EQ(result.outputChunk, 0u);
    EXPECT_EQ(result.data[0], first[4]);
    EXPECT_EQ(result.data[12], second[0]);
    EXPECT_TRUE(result.hasExtraResult);
    EXPECT_EQ(result.extraChunk, 1u);
    EXPECT_EQ(result.extraData[0], second[4]);
    EXPECT_EQ(result.extraData[12], 0);
}

TEST(TimingVeuTest, TimingProfilePreservesEvidenceProvenance)
{
    VeuTimingProfile profile;
    profile.load("src/brs/veu/testdata/veu_timing_profile_v1.csv");
    auto selected = profile.select(
        "vadd", false, "full", "src1+src2", 1,
        3, 1, 4, 4, 1, 1, 0);
    ASSERT_TRUE(selected.matched);
    EXPECT_EQ(selected.timingSource, "legacy_default");
    EXPECT_EQ(selected.controlTimingSource, "default");

    profile.load("src/brs/veu/testdata/veu_timing_profile_v2.csv");
    selected = profile.select(
        "vadd", false, "full", "src1+src2", 1,
        3, 1, 4, 4, 1, 1, 0);
    ASSERT_TRUE(selected.matched);
    EXPECT_EQ(selected.timingSource, "rtl_sim");
    EXPECT_EQ(selected.evidenceId, "legacy_v2_evidence");
    EXPECT_EQ(selected.controlTimingSource, "default");

    profile.load("src/brs/veu/testdata/veu_timing_profile_v3.csv");
    selected = profile.select(
        "vadd", false, "full", "src1+src2", 1,
        3, 1, 4, 4, 1, 1, 0);
    EXPECT_EQ(selected.operationCycles, 19u);
}

TEST(TimingVeuTest, TimingProfileRejectsInexactRtlEvidence)
{
    VeuTimingProfile profile;
    EXPECT_THROW(
        profile.load(
            "src/brs/veu/testdata/veu_timing_profile_v2_invalid_wildcard.csv"),
        std::runtime_error);
}

TEST(TimingVeuTest, LoadsExactMikuiTerminalBehavior)
{
    VeuTerminalBehavior behavior;
    behavior.load("configs/brs/veu_terminal_behavior.csv");

    const auto scalarMove = behavior.select("vmv", true, "full", 1);
    ASSERT_TRUE(scalarMove.has_value());
    EXPECT_EQ(scalarMove->classification, "RTL_TOP_DUPLICATE_ACTIVITY");
    EXPECT_EQ(scalarMove->statusClearCycles, 8u);
    EXPECT_EQ(scalarMove->lockFinishCycles, 12u);
    EXPECT_EQ(scalarMove->extraWrites, 6u);

    EXPECT_FALSE(behavior.select("vslidedown", true, "full", 1));
    EXPECT_FALSE(behavior.select("vmv", false, "full", 1));
}

TEST(TimingVeuTest, ScalarMoveReproducesMeasuredDuplicateWrites)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.terminalBehaviorPath = "configs/brs/veu_terminal_behavior.csv";
    veu.configure(config);

    std::vector<TimingVeuMemoryRequest> writes;
    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            EXPECT_TRUE(request.isWrite);
            writes.push_back(request);
            veu.completeMemoryWrite(request.transactionId);
            return true;
        });

    stepUntilResponse(veu, csrWrite(VeuCsr::Config, 0x800));
    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x20010000));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, VeuVectorBits));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, VeuFullWriteMask));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Move, 0x11223344, 0));
    stepUntilOperationComplete(veu);

    EXPECT_EQ(veu.terminalBehaviorUseCount(), 1u);
    EXPECT_EQ(veu.profileFallbackCount(), 0u);
    EXPECT_EQ(veu.busyCycleCount(), 12u);
    ASSERT_EQ(writes.size(), 7u);
    EXPECT_EQ(writes.front().address, 0x20010000u);
    EXPECT_EQ(writes[writes.size() - 2].address, 0x20010050u);
    EXPECT_EQ(writes.back().address, 0x20010050u);
}

TEST(TimingVeuTest, ScalarMoveZeroMaskIssuesOnlyMeasuredTailReads)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.terminalBehaviorPath = "configs/brs/veu_terminal_behavior.csv";
    veu.configure(config);

    std::vector<TimingVeuMemoryRequest> reads;
    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            EXPECT_FALSE(request.isWrite);
            reads.push_back(request);
            veu.completeMemoryRead(request.transactionId, makeByteVector(0));
            return true;
        });

    stepUntilResponse(veu, csrWrite(VeuCsr::Config, 0x800));
    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x20010000));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, VeuVectorBits));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Move, 0x11223344, 0x20011000));
    stepUntilOperationComplete(veu);

    EXPECT_EQ(veu.busyCycleCount(), 12u);
    EXPECT_EQ(veu.memoryReadCount(), 2u);
    EXPECT_EQ(veu.memoryWriteCount(), 0u);
    ASSERT_EQ(reads.size(), 2u);
    EXPECT_EQ(reads[0].address, 0x20011000u);
    EXPECT_EQ(reads[1].address, 0x20011010u);
}

TEST(TimingVeuTest, ZeroMaskSlideCompletesWithoutMemoryWrite)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.timingProfilePath = "configs/brs/veu_timing_profile.csv";
    veu.configure(config);

    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            EXPECT_FALSE(request.isWrite);
            veu.completeMemoryRead(request.transactionId,
                                   makeByteVector(0x10));
            return true;
        });

    stepUntilResponse(veu, csrWrite(VeuCsr::Config, 0x800));
    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x20012000));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, VeuVectorBits));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::SlideUp, 4, 0x20011000));
    stepUntilOperationComplete(veu);

    EXPECT_EQ(veu.busyCycleCount(), 15u);
    EXPECT_EQ(veu.memoryReadCount(), 1u);
    EXPECT_EQ(veu.memoryWriteCount(), 0u);
    EXPECT_EQ(veu.chunkCount(), 1u);
}

TEST(TimingVeuTest, IllegalOperationCompletesAtMeasuredTerminalCycle)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.terminalBehaviorPath = "configs/brs/veu_terminal_behavior.csv";
    veu.configure(config);
    veu.setMemoryRequestCallback(
        [](const TimingVeuMemoryRequest &) {
            ADD_FAILURE() << "illegal operation accessed memory";
            return false;
        });

    stepUntilResponse(veu, csrWrite(VeuCsr::Config, 0));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, VeuVectorBits));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, VeuFullWriteMask));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Compress, 0, 0));
    stepUntilOperationComplete(veu);

    EXPECT_EQ(veu.terminalBehaviorUseCount(), 1u);
    EXPECT_EQ(veu.illegalOperationCount(), 1u);
    EXPECT_EQ(veu.busyCycleCount(), 18u);
    EXPECT_EQ(veu.memoryReadCount(), 0u);
    EXPECT_EQ(veu.memoryWriteCount(), 0u);
}

TEST(TimingVeuTest, EveryMeasuredIllegalOperationCompletesWithoutMemory)
{
    for (const auto operation : {
             VeuInstruction::Unknown,
             VeuInstruction::Compress,
             VeuInstruction::MultiplyAdd,
             VeuInstruction::MultiplySubtract,
             VeuInstruction::MultiplyHigh,
             VeuInstruction::MultiplyHighSignedUnsigned,
             VeuInstruction::WidenReduceSum}) {
        TimingVeu veu;
        VeuTimingConfig config;
        config.terminalBehaviorPath = "configs/brs/veu_terminal_behavior.csv";
        veu.configure(config);
        veu.setMemoryRequestCallback(
            [](const TimingVeuMemoryRequest &) {
                ADD_FAILURE() << "measured illegal operation accessed memory";
                return false;
            });

        stepUntilResponse(veu, csrWrite(VeuCsr::Config, 0));
        stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, VeuVectorBits));
        stepUntilResponse(veu, csrWrite(VeuCsr::Mask, VeuFullWriteMask));
        stepUntilResponse(veu, vectorStart(operation, 0x100, 0x200));
        if (isTwoShotVeuInstruction(operation)) {
            stepUntilResponse(veu, vectorStart(operation, 0x300, 0x300));
        }
        stepUntilOperationComplete(veu);

        EXPECT_EQ(veu.terminalBehaviorUseCount(), 1u);
        EXPECT_EQ(veu.illegalOperationCount(), 1u);
        EXPECT_EQ(veu.busyCycleCount(), 18u);
        EXPECT_EQ(veu.memoryReadCount(), 0u);
        EXPECT_EQ(veu.memoryWriteCount(), 0u);
    }
}

TEST(TimingVeuTest, TerminalTailReadDrainsAfterVisibleCompletion)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.terminalBehaviorPath = "configs/brs/veu_terminal_behavior.csv";
    veu.configure(config);

    uint64_t delayedTransaction = 0;
    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            if (request.isWrite) {
                veu.completeMemoryWrite(request.transactionId);
            } else if (request.chunkIndex == 9) {
                delayedTransaction = request.transactionId;
            } else {
                veu.completeMemoryRead(request.transactionId,
                                       makeByteVector(0x10));
            }
            return true;
        });

    stepUntilResponse(veu, csrWrite(VeuCsr::Config, 0x800));
    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x500));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength,
                                    8 * VeuVectorBits));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, VeuFullWriteMask));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::SlideDown, 4, 0x300));
    stepUntilOperationComplete(veu);

    ASSERT_NE(delayedTransaction, 0u);
    EXPECT_EQ(veu.terminalBehaviorUseCount(), 1u);
    EXPECT_EQ(veu.busyCycleCount(), 28u);
    EXPECT_FALSE(veu.quiescent());

    veu.completeMemoryRead(delayedTransaction, makeByteVector(0x20));
    EXPECT_TRUE(veu.quiescent());
    EXPECT_EQ(veu.unexpectedResponseCount(), 0u);
}

} // namespace
} // namespace brs
} // namespace gem5
