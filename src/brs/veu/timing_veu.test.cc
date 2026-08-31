#include "brs/veu/timing_veu.hh"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
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

std::array<uint8_t, VeuVectorBytes>
makeElementVector(uint32_t value, uint32_t bytes)
{
    std::array<uint8_t, VeuVectorBytes> data = {};
    for (uint32_t offset = 0; offset < VeuVectorBytes; offset += bytes) {
        for (uint32_t byte = 0; byte < bytes; ++byte) {
            data[offset + byte] =
                static_cast<uint8_t>((value >> (byte * 8)) & 0xff);
        }
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

uint32_t
element(const std::array<uint8_t, VeuVectorBytes> &data, uint32_t index,
        uint32_t bytes)
{
    uint32_t value = 0;
    for (uint32_t byte = 0; byte < bytes; ++byte) {
        value |= static_cast<uint32_t>(data[index * bytes + byte]) <<
                 (byte * 8);
    }
    return value;
}

std::vector<std::string>
splitTestCsv(const std::string &line)
{
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) {
        if (!field.empty() && field.back() == '\r') {
            field.pop_back();
        }
        fields.push_back(field);
    }
    if (!line.empty() && line.back() == ',') {
        fields.emplace_back();
    }
    return fields;
}

uint32_t
parseHex32(const std::string &text)
{
    return static_cast<uint32_t>(std::stoul(text, nullptr, 16));
}

VeuVector
parseRtlVector(const std::string &text)
{
    VeuVector result = {};
    if (text.empty()) {
        return result;
    }
    EXPECT_EQ(text.size(), VeuVectorBytes * 2);
    if (text.size() != VeuVectorBytes * 2) {
        return result;
    }
    for (uint32_t byte = 0; byte < VeuVectorBytes; ++byte) {
        const size_t position = text.size() - (byte + 1) * 2;
        result[byte] = static_cast<uint8_t>(
            std::stoul(text.substr(position, 2), nullptr, 16));
    }
    return result;
}

VeuInstruction
parseRtlOperation(const std::string &op)
{
    static const std::map<std::string, VeuInstruction> operations = {
        {"vadd", VeuInstruction::Add},
        {"vsub", VeuInstruction::Sub},
        {"vmin", VeuInstruction::Min},
        {"vmax", VeuInstruction::Max},
        {"vand", VeuInstruction::And},
        {"vor", VeuInstruction::Or},
        {"vxor", VeuInstruction::Xor},
        {"vredsum", VeuInstruction::ReduceSum},
        {"vmul", VeuInstruction::Multiply},
        {"vssrl", VeuInstruction::ShiftRightLogical},
        {"vssra", VeuInstruction::ShiftRightArithmetic},
        {"vnclip", VeuInstruction::NarrowClip},
        {"vmv", VeuInstruction::Move},
        {"vslideup", VeuInstruction::SlideUp},
        {"vslidedown", VeuInstruction::SlideDown},
        {"vredmin", VeuInstruction::ReduceMin},
        {"vredmax", VeuInstruction::ReduceMax},
        {"vmac", VeuInstruction::MultiplyAdd},
        {"vmsub", VeuInstruction::MultiplySubtract},
        {"vwredsum", VeuInstruction::WidenReduceSum},
        {"vmulh", VeuInstruction::MultiplyHigh},
    };
    const auto found = operations.find(op);
    EXPECT_NE(found, operations.end()) << "unknown captured RTL op " << op;
    return found == operations.end() ? VeuInstruction::Unknown :
                                       found->second;
}

uint32_t
compareCapturedRtlVectors(const std::string &path)
{
    std::ifstream input(path);
    if (!input) {
        return 0;
    }

    std::string line;
    EXPECT_TRUE(static_cast<bool>(std::getline(input, line)));
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    const bool handoffSchema =
        line.rfind("case_id,operation,", 0) == 0;
    if (!handoffSchema) {
        EXPECT_EQ(
            line,
            "test,op,config,mode,scalar_en,scalar_value,shift,vlen,mask,"
            "chunk,src1,src2,src3,result,wstrb,dst_addr");
    }

    VeuFunctionalExecutor executor;
    std::string activeTest;
    std::map<uint32_t, VeuVector> expectedByChunk;
    std::map<uint32_t, bool> expectedWriteByChunk;
    uint32_t compared = 0;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const auto fields = splitTestCsv(line);
        const size_t expectedFields = handoffSchema ? 19 : 16;
        EXPECT_EQ(fields.size(), expectedFields);
        if (fields.size() != expectedFields) {
            return compared;
        }
        const size_t vlenField = handoffSchema ? 7 : 7;
        const size_t maskField = handoffSchema ? 9 : 8;
        const size_t chunkField = handoffSchema ? 10 : 9;
        const size_t source1Field = handoffSchema ? 11 : 10;
        const size_t source2Field = handoffSchema ? 12 : 11;
        const size_t source3Field = handoffSchema ? 13 : 12;
        const size_t resultField = handoffSchema ? 14 : 13;
        const size_t strobeField = handoffSchema ? 15 : 14;
        if (fields[0] != activeTest) {
            activeTest = fields[0];
            executor.reset();
            expectedByChunk.clear();
            expectedWriteByChunk.clear();
        }

        VeuFunctionalInput functionalInput;
        functionalInput.instruction = parseRtlOperation(fields[1]);
        functionalInput.config = parseHex32(fields[2]);
        functionalInput.scalar = parseHex32(fields[5]);
        functionalInput.chunkCount =
            static_cast<uint32_t>(std::stoul(fields[vlenField])) /
            VeuVectorBits;
        functionalInput.chunkIndex =
            static_cast<uint32_t>(std::stoul(fields[chunkField]));
        functionalInput.writeMask = parseHex32(fields[maskField]);
        functionalInput.source1 = parseRtlVector(fields[source1Field]);
        functionalInput.source2 = parseRtlVector(fields[source2Field]);
        functionalInput.source3 = parseRtlVector(fields[source3Field]);

        const auto expected = parseRtlVector(fields[resultField]);
        const bool expectedWrite = parseHex32(fields[strobeField]) != 0;
        expectedByChunk[functionalInput.chunkIndex] = expected;
        expectedWriteByChunk[functionalInput.chunkIndex] = expectedWrite;
        const auto actual = executor.execute(functionalInput);
        if (functionalInput.instruction == VeuInstruction::SlideDown &&
            functionalInput.chunkCount > 1) {
            if (actual.writeResult) {
                EXPECT_EQ(actual.data, expectedByChunk[actual.outputChunk])
                    << "test=" << fields[0]
                    << " output_chunk=" << actual.outputChunk;
                EXPECT_EQ(actual.writeResult,
                          expectedWriteByChunk[actual.outputChunk]);
            }
            if (actual.hasExtraResult) {
                EXPECT_EQ(actual.extraData,
                          expectedByChunk[actual.extraChunk])
                    << "test=" << fields[0]
                    << " extra_chunk=" << actual.extraChunk;
                EXPECT_TRUE(expectedWriteByChunk[actual.extraChunk]);
            }
        } else {
            EXPECT_EQ(actual.data, expected)
                << "test=" << fields[0] << " chunk=" << fields[chunkField];
            EXPECT_EQ(actual.writeResult, expectedWrite)
                << "test=" << fields[0] << " chunk=" << fields[chunkField];
        }
        ++compared;
    }
    return compared;
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

TEST(TimingVeuTest, VectorStartReturnsPreWriteCsrValueLikeRtlVcu)
{
    TimingVeu veu;

    veu.clock(csrWrite(VeuCsr::ReadAddress1, 0x12345678u));
    ASSERT_TRUE(veu.evaluate().valid);
    veu.clock({});

    veu.clock(vectorStart(VeuInstruction::Add, 0x100u, 0x200u));
    ASSERT_TRUE(veu.evaluate().valid);
    EXPECT_EQ(veu.evaluate().readData, 0x12345678u);
}

TEST(TimingVeuTest, AcceptsNextControlRequestWhileResponding)
{
    TimingVeu veu;

    veu.clock(csrWrite(VeuCsr::WriteAddress, 0x400u));
    ASSERT_TRUE(veu.evaluate().valid);

    // The response for the write and registration of the following read
    // share one edge, matching the VCU csr_valid pipeline throughput.
    veu.clock(csrRead(VeuCsr::WriteAddress));
    ASSERT_TRUE(veu.evaluate().valid);
    EXPECT_EQ(veu.evaluate().readData, 0x400u);

    veu.clock({});
    EXPECT_FALSE(veu.evaluate().valid);
}

TEST(TimingVeuTest, ResetMaskIsZeroAndSuppressesUnconfiguredWrite)
{
    TimingVeu veu;
    std::map<uint32_t, VeuVector> memory;
    memory[0x100] = makeByteVector(1);
    memory[0x200] = makeByteVector(2);
    memory[0x300] = makeByteVector(0xaa);
    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            if (request.isWrite) {
                memory[request.address] = request.data;
                veu.completeMemoryWrite(request.transactionId);
            } else {
                veu.completeMemoryRead(request.transactionId,
                                       memory[request.address]);
            }
            return true;
        });

    EXPECT_EQ(veu.csrMask(), 0u);
    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x300));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 256));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));
    stepUntilOperationComplete(veu);

    EXPECT_EQ(memory[0x300], makeByteVector(0xaa));
    EXPECT_EQ(veu.memoryWriteCount(), 0u);
    EXPECT_EQ(veu.zeroMaskSkippedWriteCount(), 1u);
}

TEST(TimingVeuTest, VectorLengthCsrRoundsLikeRtl)
{
    TimingVeu veu;
    for (const auto &[requested, expected] :
         std::array<std::pair<uint32_t, uint32_t>, 4>{{
             {1, 256}, {256, 256}, {257, 512}, {2048, 2048}}}) {
        stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, requested));
        EXPECT_EQ(veu.csrVectorLength(), expected) << "requested=" << requested;
    }
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
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
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

TEST(TimingVeuTest, DefaultStructuralBoundariesMatchRtlEdges)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.executeLatency = 1;
    config.vsuLatency = 1;
    veu.configure(config);

    std::map<uint32_t, std::array<uint8_t, VeuVectorBytes>> memory;
    memory[0x100] = makeVector({1, 2, 3, 4, 5, 6, 7, 8});
    memory[0x200] = makeVector({10, 20, 30, 40, 50, 60, 70, 80});

    std::vector<uint64_t> readIssueCycles;
    uint64_t writeIssueCycle = 0;
    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            if (request.isWrite) {
                writeIssueCycle = veu.busyCycleCount();
                veu.completeMemoryWrite(request.transactionId);
            } else {
                readIssueCycles.push_back(veu.busyCycleCount());
                veu.completeMemoryRead(request.transactionId,
                                       memory[request.address]);
            }
            return true;
        });

    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x300));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, VeuVectorBits));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));

    while (veu.vfuAcceptedCount() == 0) {
        veu.clock({});
    }

    ASSERT_EQ(readIssueCycles.size(), 2u);
    EXPECT_EQ(readIssueCycles[0], 5u);
    EXPECT_EQ(readIssueCycles[1], 6u);
    // The second response is committed on busy cycle 7. The VFU may only
    // consume it across the FIFO boundary on the following edge.
    EXPECT_EQ(veu.busyCycleCount(), 8u);

    stepUntilOperationComplete(veu);
    EXPECT_EQ(writeIssueCycle, 10u);
    // The immediate write response is observed on cycle 11, followed by the
    // four complete RTL finish/drain cycles.
    EXPECT_EQ(veu.busyCycleCount(), 15u);
}

TEST(TimingVeuTest, MultiplyIssuesSource2BeforeSource1LikeRtl)
{
    TimingVeu veu;
    std::vector<VeuSource> readSources;
    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            if (request.isWrite) {
                veu.completeMemoryWrite(request.transactionId);
            } else {
                readSources.push_back(request.source);
                veu.completeMemoryRead(request.transactionId,
                                       makeByteVector(2));
            }
            return true;
        });

    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x300));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 256));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Multiply, 0x100, 0x200));
    while (readSources.size() < 2) {
        veu.clock({});
    }

    ASSERT_EQ(readSources.size(), 2u);
    EXPECT_EQ(readSources[0], VeuSource::Source2);
    EXPECT_EQ(readSources[1], VeuSource::Source1);
    stepUntilOperationComplete(veu);
}

TEST(TimingVeuTest, StatusClearsBeforeLockFinishAtRtlBoundary)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.executeLatency = 1;
    config.vsuLatency = 1;
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
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 256));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));

    EXPECT_TRUE(veu.lockIsActive());
    EXPECT_EQ(veu.busyCycleCount(), 1u);

    uint64_t statusClearCycle = 0;
    uint64_t lockFinishCycle = 0;
    while (veu.operationBusy()) {
        const bool statusWasBusy = veu.statusIsBusy();
        const bool lockWasActive = veu.lockIsActive();
        veu.clock({});
        if (statusWasBusy && !veu.statusIsBusy()) {
            statusClearCycle = veu.busyCycleCount();
        }
        if (lockWasActive && !veu.lockIsActive()) {
            lockFinishCycle = veu.busyCycleCount();
        }
    }
    EXPECT_NE(statusClearCycle, 0u);
    EXPECT_EQ(lockFinishCycle - statusClearCycle, 4u);
    EXPECT_EQ(lockFinishCycle, 15u);
    EXPECT_EQ(veu.statusActiveCycleCount(), statusClearCycle);
    EXPECT_EQ(veu.lockActiveCycleCount(), lockFinishCycle - 1);
}

TEST(TimingVeuTest, StatusReadOnClearEdgeReturnsPreClearBusyValue)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.executeLatency = 1;
    config.vsuLatency = 1;
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
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 256));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));

    bool observedClearEdge = false;
    while (veu.operationBusy()) {
        const bool statusWasBusy = veu.statusIsBusy();
        veu.clock(csrRead(VeuCsr::Status));
        if (statusWasBusy && !veu.statusIsBusy()) {
            observedClearEdge = true;
            ASSERT_TRUE(veu.evaluate().valid);
            EXPECT_EQ(veu.evaluate().readData & 1u, 1u);
        }
    }
    EXPECT_TRUE(observedClearEdge);
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
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
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
    EXPECT_EQ(veu.csrReadAddress1(), 0x120u);
    EXPECT_EQ(veu.csrReadAddress2(), 0x220u);
    EXPECT_EQ(veu.csrReadAddress3(), 0x420u);
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
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
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

TEST(TimingVeuTest, VisibleCsrsAdvancePerAcceptedVectorRequest)
{
    TimingVeu veu;
    std::map<uint32_t, VeuVector> memory;
    memory[0x100] = memory[0x120] = makeByteVector(1);
    memory[0x200] = memory[0x220] = makeByteVector(2);
    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
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
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));

    uint64_t observedReads = 0;
    while (veu.operationBusy()) {
        veu.clock({});
        if (veu.memoryReadCount() == observedReads) {
            continue;
        }
        observedReads = veu.memoryReadCount();
        if (observedReads == 1) {
            EXPECT_EQ(veu.csrReadAddress1(), 0x120u);
            EXPECT_EQ(veu.csrReadAddress2(), 0x200u);
        } else if (observedReads == 2) {
            EXPECT_EQ(veu.csrReadAddress2(), 0x220u);
        } else if (observedReads == 3) {
            EXPECT_EQ(veu.csrReadAddress1(), 0x140u);
        } else if (observedReads == 4) {
            EXPECT_EQ(veu.csrReadAddress2(), 0x240u);
        }
    }

    EXPECT_EQ(veu.csrVectorLength(), 0u);
    EXPECT_EQ(veu.csrWriteAddress(), 0x340u);
    EXPECT_EQ(memory[0x300], makeByteVector(3));
    EXPECT_EQ(memory[0x320], makeByteVector(3));

    const uint64_t readsBeforeRestart = veu.memoryReadCount();
    const uint64_t writesBeforeRestart = veu.memoryWriteCount();
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));
    EXPECT_FALSE(veu.operationBusy());
    EXPECT_EQ(veu.startedOperationCount(), 1u);
    EXPECT_EQ(veu.zeroLengthNoopCount(), 1u);
    EXPECT_EQ(veu.memoryReadCount(), readsBeforeRestart);
    EXPECT_EQ(veu.memoryWriteCount(), writesBeforeRestart);
}

TEST(TimingVeuTest, ScalarSourceDoesNotAdvanceReadAddress1)
{
    TimingVeu veu;
    std::map<uint32_t, VeuVector> memory;
    memory[0x200] = memory[0x220] = makeByteVector(2);
    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            if (request.isWrite) {
                veu.completeMemoryWrite(request.transactionId);
            } else {
                veu.completeMemoryRead(request.transactionId,
                                       memory[request.address]);
            }
            return true;
        });

    stepUntilResponse(veu, csrWrite(VeuCsr::WriteAddress, 0x300));
    stepUntilResponse(veu, csrWrite(VeuCsr::Config, 0x800));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 512));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x01010101, 0x200));
    stepUntilOperationComplete(veu);

    EXPECT_EQ(veu.csrReadAddress1(), 0x01010101u);
    EXPECT_EQ(veu.csrReadAddress2(), 0x240u);
    EXPECT_EQ(veu.memoryReadCount(), 2u);
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
    EXPECT_EQ(veu.csrVectorLength(), 0u);
    EXPECT_EQ(veu.csrWriteAddress(), 0x320u);
}

TEST(TimingVeuTest, ReductionAccumulatesAcrossChunksAndWritesOnlyFinalResult)
{
    TimingVeu veu;
    std::map<uint32_t, VeuVector> memory;
    memory[0x100] = makeByteVector(2);
    memory[0x120] = makeByteVector(2);
    uint64_t writes = 0;
    uint32_t writeChunk = 0xffffffffu;
    veu.setMemoryRequestCallback(
        [&](const TimingVeuMemoryRequest &request) {
            if (request.isWrite) {
                ++writes;
                writeChunk = request.chunkIndex;
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
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::ReduceSum, 0x100, 0));
    stepUntilOperationComplete(veu, 200);

    EXPECT_EQ(writes, 1u);
    EXPECT_EQ(writeChunk, 0u);
    EXPECT_EQ(lane(memory[0x300], 0), 128u);
    EXPECT_EQ(veu.memoryReadCount(), 2u);
    EXPECT_EQ(veu.csrWriteAddress(), 0x300u);
    EXPECT_EQ(veu.csrVectorLength(), 0u);
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
        stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
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
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));
    stepUntilOperationComplete(veu, 500);

    EXPECT_GE(veu.retryCount(), 1u);
    EXPECT_EQ(veu.storePriorityCount(), 8u);
    EXPECT_GT(veu.readBlockedByStoreCount(), 0u);
    EXPECT_LT(veu.readBlockedByStoreCount(), veu.storePriorityCount());
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
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Move, 0x100, 0));
    stepUntilOperationComplete(veu, 200);

    EXPECT_EQ(veu.memoryReadCount(), 2u);
    EXPECT_EQ(veu.memoryWriteCount(), 2u);
    EXPECT_EQ(veu.retryCount(), 1u);
    EXPECT_EQ(veu.storePriorityCount(), 3u);
    EXPECT_EQ(veu.readBlockedByStoreCount(), 0u);
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
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
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

TEST(TimingVeuTest, SaturatingArithmeticMatchesRtlModes)
{
    struct Case
    {
        uint32_t mode;
        uint32_t bytes;
        uint32_t lhs;
        uint32_t rhs;
        uint32_t expected;
    };
    const std::array<Case, 4> addCases = {{
        {0, 1, 0xfe, 0x02, 0xff},
        {1, 2, 0xfffe, 0x0002, 0xffff},
        {2, 1, 0x7f, 0x01, 0x7f},
        {3, 2, 0x7fff, 0x0001, 0x7fff},
    }};
    for (const auto &test : addCases) {
        VeuFunctionalExecutor executor;
        VeuFunctionalInput input;
        input.config = test.mode << 7;
        input.instruction = VeuInstruction::Add;
        input.source1 = makeElementVector(test.lhs, test.bytes);
        input.source2 = makeElementVector(test.rhs, test.bytes);
        const auto result = executor.execute(input);
        EXPECT_EQ(element(result.data, 0, test.bytes), test.expected)
            << "mode=" << test.mode;
    }

    VeuFunctionalExecutor executor;
    VeuFunctionalInput input;
    input.config = 2u << 7;
    input.instruction = VeuInstruction::Sub;
    input.source1 = makeByteVector(0x80);
    input.source2 = makeByteVector(1);
    EXPECT_EQ(executor.execute(input).data[0], 0x80u);
}

TEST(TimingVeuTest, ScalarWordReplicationMatchesCapturedRtlVector)
{
    VeuFunctionalExecutor executor;
    VeuFunctionalInput input;
    input.config = 0x800 | (2u << 7);
    input.scalar = 0x12345678;
    input.instruction = VeuInstruction::Add;
    input.source2 = makeByteVector(1);
    const auto result = executor.execute(input);
    for (uint32_t word = 0; word < VeuLaneCount; ++word) {
        EXPECT_EQ(lane(result.data, word), 0x13355779u);
    }
}

TEST(TimingVeuTest, MultiplyUsesShiftFigBeforeSaturation)
{
    VeuFunctionalExecutor executor;
    VeuFunctionalInput input;
    input.config = (2u << 7) | (1u << 1);
    input.instruction = VeuInstruction::Multiply;
    input.source1 = makeByteVector(4);
    input.source2 = makeByteVector(4);
    EXPECT_EQ(executor.execute(input).data[0], 8u);

    executor.reset();
    input.config = 2u << 7;
    input.source1 = makeByteVector(0x40);
    input.source2 = makeByteVector(4);
    EXPECT_EQ(executor.execute(input).data[0], 0x7fu);
}

TEST(TimingVeuTest, ShiftAmountIsSharedWithinEachRtlWord)
{
    VeuFunctionalExecutor executor;
    VeuFunctionalInput input;
    input.config = 2u << 7;
    input.instruction = VeuInstruction::ShiftRightArithmetic;
    input.source1 = makeVector({1, 2, 3, 4, 5, 6, 7, 8});
    input.source2 = makeByteVector(0x80);
    const auto result = executor.execute(input);
    EXPECT_EQ(result.data[0], 0xc0u);
    EXPECT_EQ(result.data[1], 0xc0u);
    EXPECT_EQ(result.data[4], 0xe0u);

    executor.reset();
    input.config = 0;
    input.source1 = makeVector({1, 1, 1, 1, 1, 1, 1, 1});
    EXPECT_EQ(executor.execute(input).data[0], 0x40u);
}

TEST(TimingVeuTest, VectorNarrowClipUsesBoundsFromEachSourceWord)
{
    VeuFunctionalExecutor executor;
    VeuFunctionalInput input;
    input.config = 0;
    input.instruction = VeuInstruction::NarrowClip;
    input.source1 = makeVector({0x00400005, 0x00300010, 0x00200015,
                                0x001f001a, 0x00400005, 0x00300010,
                                0x00200015, 0x001f001a});
    input.source2 = makeByteVector(0xff);
    const auto result = executor.execute(input);
    EXPECT_EQ(lane(result.data, 0), 0x40404040u);
    EXPECT_EQ(lane(result.data, 1), 0x30303030u);
}

TEST(TimingVeuTest, SlideMaintainsRtlCrossChunkState)
{
    VeuVector first = {};
    VeuVector second = {};
    for (uint32_t index = 0; index < VeuVectorBytes; ++index) {
        first[index] = static_cast<uint8_t>(index);
        second[index] = static_cast<uint8_t>(0x80 + index);
    }

    VeuFunctionalExecutor executor;
    VeuFunctionalInput input;
    input.config = 0x800;
    input.scalar = 1;
    input.chunkCount = 2;
    input.instruction = VeuInstruction::SlideUp;
    input.source2 = first;
    auto result = executor.execute(input);
    EXPECT_EQ(result.data[0], 0u);
    EXPECT_EQ(result.data[1], 0u);
    input.chunkIndex = 1;
    input.source2 = second;
    result = executor.execute(input);
    EXPECT_EQ(result.data[0], first[VeuVectorBytes - 1]);
    EXPECT_EQ(result.data[1], second[0]);

    executor.reset();
    input.chunkIndex = 0;
    input.instruction = VeuInstruction::SlideDown;
    input.source2 = first;
    result = executor.execute(input);
    EXPECT_FALSE(result.writeResult);
    input.chunkIndex = 1;
    input.source2 = second;
    result = executor.execute(input);
    EXPECT_TRUE(result.writeResult);
    EXPECT_EQ(result.outputChunk, 0u);
    EXPECT_EQ(result.data[0], first[1]);
    EXPECT_EQ(result.data[VeuVectorBytes - 1], second[0]);
    ASSERT_TRUE(result.hasExtraResult);
    EXPECT_EQ(result.extraChunk, 1u);
    EXPECT_EQ(result.extraData[0], second[1]);
    EXPECT_EQ(result.extraData[VeuVectorBytes - 1], 0u);
}

TEST(TimingVeuTest, OperationDescriptionMatchesRtlSourceSelection)
{
    const auto scalarSub =
        VeuFunctionalExecutor::describe(VeuInstruction::Sub, true);
    EXPECT_TRUE(VeuFunctionalExecutor::sourceRequired(
        scalarSub.sourceMask, VeuSource::Source2));
    EXPECT_FALSE(VeuFunctionalExecutor::sourceRequired(
        scalarSub.sourceMask, VeuSource::Source1));

    const auto scalarMac =
        VeuFunctionalExecutor::describe(VeuInstruction::MultiplyAdd, true);
    EXPECT_FALSE(VeuFunctionalExecutor::sourceRequired(
        scalarMac.sourceMask, VeuSource::Source1));
    EXPECT_TRUE(VeuFunctionalExecutor::sourceRequired(
        scalarMac.sourceMask, VeuSource::Source2));
    EXPECT_TRUE(VeuFunctionalExecutor::sourceRequired(
        scalarMac.sourceMask, VeuSource::Source3));

    const auto multiplyHigh =
        VeuFunctionalExecutor::describe(VeuInstruction::MultiplyHigh, false);
    EXPECT_TRUE(multiplyHigh.supported);
    EXPECT_FALSE(multiplyHigh.rtlIllegal);

    const auto compress =
        VeuFunctionalExecutor::describe(VeuInstruction::Compress, false);
    EXPECT_FALSE(compress.supported);
    EXPECT_TRUE(compress.rtlIllegal);
}

TEST(TimingVeuTest, FunctionalResultsMatchAllCapturedRtlVectors)
{
    const char *configuredRoot = std::getenv("BRS_VEU_RTL_RESULTS_DIR");
    const std::string resultsRoot = configuredRoot ?
        configuredRoot : "../../veu_timing_results";
    const std::string fixedVectors =
        resultsRoot +
        "/yinglong_veu_timing_fixed2/veu_functional_vectors.csv";
    const std::string coverageVectors =
        resultsRoot +
        "/yinglong_veu_timing_coverage/veu_functional_vectors.csv";

    std::ifstream fixedProbe(fixedVectors);
    std::ifstream coverageProbe(coverageVectors);
    if (!fixedProbe || !coverageProbe) {
        GTEST_SKIP()
            << "RTL capture files are external; set "
               "BRS_VEU_RTL_RESULTS_DIR to run the differential test";
    }
    fixedProbe.close();
    coverageProbe.close();

    const uint32_t fixedCompared =
        compareCapturedRtlVectors(fixedVectors);
    const uint32_t coverageCompared =
        compareCapturedRtlVectors(coverageVectors);
    EXPECT_EQ(fixedCompared, 135u);
    EXPECT_EQ(coverageCompared, 22u);

    const char *configuredGapRoot = std::getenv("BRS_VEU_RTL_GAP_DIR");
    const std::string gapRoot = configuredGapRoot ?
        configuredGapRoot : "../../rtl_test_case";
    const std::string gapVectors =
        gapRoot + "/veu_functional_vectors.csv";
    std::ifstream gapProbe(gapVectors);
    if (gapProbe) {
        gapProbe.close();
        EXPECT_EQ(compareCapturedRtlVectors(gapVectors), 136u);
    }
}

TEST(TimingVeuTest, ReductionMinMaxExposeRunningResultAndC2DoubleWrite)
{
    for (const auto instruction :
         {VeuInstruction::ReduceMin, VeuInstruction::ReduceMax}) {
        VeuFunctionalExecutor executor;
        VeuFunctionalInput input;
        input.instruction = instruction;
        input.config = 0x700;
        input.chunkCount = 2;
        input.source1 = makeByteVector(
            instruction == VeuInstruction::ReduceMin ? 9 : 2);

        input.chunkIndex = 0;
        const auto first = executor.execute(input);
        EXPECT_TRUE(first.writeResult);
        EXPECT_EQ(first.data[0],
                  instruction == VeuInstruction::ReduceMin ? 9u : 2u);

        input.chunkIndex = 1;
        input.source1 = makeByteVector(
            instruction == VeuInstruction::ReduceMin ? 3 : 7);
        const auto second = executor.execute(input);
        EXPECT_TRUE(second.writeResult);
        EXPECT_EQ(second.data[0],
                  instruction == VeuInstruction::ReduceMin ? 3u : 7u);
    }

    VeuFunctionalExecutor executor;
    VeuFunctionalInput input;
    input.instruction = VeuInstruction::ReduceMax;
    input.config = 0x700;
    input.chunkCount = 4;
    input.source1 = makeByteVector(5);
    const auto running = executor.execute(input);
    EXPECT_FALSE(running.writeResult);
    EXPECT_EQ(running.data[0], 5u);
}

TEST(TimingVeuTest, TimingProfileV4UsesMeasuredTimingAcrossModes)
{
    for (uint32_t mode = 0; mode < 4; ++mode) {
        TimingVeu veu;
        VeuTimingConfig config;
        config.timingProfilePath = "configs/brs/veu_timing_profile.csv";
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
        stepUntilResponse(veu, csrWrite(VeuCsr::Config, mode << 7));
        stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 256));
        stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
        stepUntilResponse(veu,
            vectorStart(VeuInstruction::Sub, 0x100, 0x200));

        EXPECT_EQ(veu.activeTimingProfileId(), "rtl_vsub_c1")
            << "mode=" << mode;
        EXPECT_EQ(veu.activeTimingSource(), "rtl_sim") << "mode=" << mode;
        EXPECT_EQ(veu.activeTimingEvidenceId(),
                  "yinglong_veu_timing_fixed2_006_vsub_vlen256")
            << "mode=" << mode;
        EXPECT_EQ(veu.activeControlTimingSource(), "rtl_sim")
            << "mode=" << mode;
        EXPECT_EQ(veu.activeControlTimingEvidenceId(),
                  "yinglong_veu_timing_fixed2_006_vsub_vlen256")
            << "mode=" << mode;
        EXPECT_EQ(veu.activeExecuteLatency(), 1u) << "mode=" << mode;
        EXPECT_EQ(veu.activeLockStartDelay(), 1u) << "mode=" << mode;
        EXPECT_EQ(veu.activeFinishDrainCycles(), 4u) << "mode=" << mode;
        EXPECT_EQ(veu.activeOperationCycles(), 18u) << "mode=" << mode;
        EXPECT_EQ(veu.profileHitCount(), 1u) << "mode=" << mode;
        EXPECT_EQ(veu.profileFallbackCount(), 0u) << "mode=" << mode;
        EXPECT_EQ(veu.rtlSimTimingUseCount(), 1u) << "mode=" << mode;
        EXPECT_EQ(veu.legacyTimingUseCount(), 0u) << "mode=" << mode;
        EXPECT_EQ(veu.defaultTimingUseCount(), 0u) << "mode=" << mode;
        EXPECT_EQ(veu.rtlSimControlTimingUseCount(), 1u) << "mode=" << mode;
        EXPECT_EQ(veu.defaultControlTimingUseCount(), 0u) << "mode=" << mode;
    }
}

TEST(TimingVeuTest, TimingProfileV4ContainsCompleteRtlGapImport)
{
    std::ifstream input("configs/brs/veu_timing_profile.csv");
    ASSERT_TRUE(input);
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(input, line)));
    uint32_t rows = 0;
    uint32_t gapRows = 0;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        ++rows;
        if (line.find("veu_rtl_gap_measured_20260727/") !=
            std::string::npos) {
            ++gapRows;
        }
    }
    EXPECT_EQ(rows, 64u);
    EXPECT_EQ(gapRows, 34u);
}

TEST(TimingVeuTest, TimingProfileV4FallsBackForUnmeasuredMaskClass)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.timingProfilePath = "configs/brs/veu_timing_profile.csv";
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
    stepUntilResponse(veu, csrWrite(VeuCsr::Config, 0));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 256));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0x0000ffffu));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Sub, 0x100, 0x200));

    EXPECT_EQ(veu.activeTimingProfileId(), "fallback");
    EXPECT_EQ(veu.activeTimingSource(), "default");
    EXPECT_EQ(veu.activeControlTimingSource(), "default");
    EXPECT_EQ(veu.activeExecuteLatency(), VeuComputeDelayCycles);
    EXPECT_EQ(veu.activeLockStartDelay(), 1u);
    EXPECT_EQ(veu.activeFinishDrainCycles(), 4u);
    EXPECT_EQ(veu.profileFallbackCount(), 1u);
    EXPECT_EQ(veu.rtlSimTimingUseCount(), 0u);
    EXPECT_EQ(veu.defaultTimingUseCount(), 1u);
    EXPECT_EQ(veu.rtlSimControlTimingUseCount(), 0u);
    EXPECT_EQ(veu.defaultControlTimingUseCount(), 1u);
}

TEST(TimingVeuTest, TimingProfileV4DoesNotSpreadAcrossChunkClasses)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.timingProfilePath = "configs/brs/veu_timing_profile.csv";
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
    stepUntilResponse(veu, csrWrite(VeuCsr::Config, 2u << 7));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 512));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Sub, 0x100, 0x200));

    EXPECT_EQ(veu.activeTimingProfileId(), "fallback");
    EXPECT_EQ(veu.activeTimingSource(), "default");
    EXPECT_EQ(veu.profileHitCount(), 0u);
    EXPECT_EQ(veu.profileFallbackCount(), 1u);
    EXPECT_EQ(veu.defaultTimingUseCount(), 1u);
    EXPECT_EQ(veu.defaultControlTimingUseCount(), 1u);
}

TEST(TimingVeuTest, TimingProfileV1LoadsAsLegacyDefault)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.timingProfilePath =
        "src/brs/veu/testdata/veu_timing_profile_v1.csv";
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
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));

    EXPECT_EQ(veu.activeTimingProfileId(), "legacy_vadd");
    EXPECT_EQ(veu.activeTimingSource(), "legacy_default");
    EXPECT_EQ(veu.activeTimingEvidenceId(), "legacy:legacy_vadd");
    EXPECT_EQ(veu.activeControlTimingSource(), "default");
    EXPECT_EQ(veu.activeControlTimingEvidenceId(),
              "builtin_veu_timing_config");
    EXPECT_EQ(veu.activeExecuteLatency(), 2u);
    EXPECT_EQ(veu.legacyTimingUseCount(), 1u);
    EXPECT_EQ(veu.defaultControlTimingUseCount(), 1u);
}

TEST(TimingVeuTest, TimingProfileV2UsesDefaultControlTiming)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.timingProfilePath =
        "src/brs/veu/testdata/veu_timing_profile_v2.csv";
    config.lockStartDelayCycles = 2;
    config.finishCycles = 6;
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
    stepUntilResponse(veu, csrWrite(VeuCsr::Config, 2u << 7));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 256));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));

    EXPECT_EQ(veu.activeTimingProfileId(), "legacy_v2_vadd");
    EXPECT_EQ(veu.activeTimingSource(), "rtl_sim");
    EXPECT_EQ(veu.activeControlTimingSource(), "default");
    EXPECT_EQ(veu.activeLockStartDelay(), 2u);
    EXPECT_EQ(veu.activeFinishDrainCycles(), 6u);
    EXPECT_EQ(veu.rtlSimTimingUseCount(), 1u);
    EXPECT_EQ(veu.defaultControlTimingUseCount(), 1u);
}

TEST(TimingVeuTest, TimingProfileV3KeepsOperationCyclesAndDefaultControl)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.timingProfilePath =
        "src/brs/veu/testdata/veu_timing_profile_v3.csv";
    config.lockStartDelayCycles = 2;
    config.finishCycles = 6;
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
    stepUntilResponse(veu, csrWrite(VeuCsr::Config, 2u << 7));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 256));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));

    EXPECT_EQ(veu.activeTimingProfileId(), "legacy_v3_vadd");
    EXPECT_EQ(veu.activeTimingSource(), "rtl_sim");
    EXPECT_EQ(veu.activeControlTimingSource(), "default");
    EXPECT_EQ(veu.activeLockStartDelay(), 2u);
    EXPECT_EQ(veu.activeFinishDrainCycles(), 6u);
    EXPECT_EQ(veu.activeOperationCycles(), 19u);
    EXPECT_EQ(veu.rtlSimTimingUseCount(), 1u);
    EXPECT_EQ(veu.defaultControlTimingUseCount(), 1u);
}

TEST(TimingVeuTest, TimingProfileV2AllowsModeWildcardRtlEvidence)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.timingProfilePath =
        "src/brs/veu/testdata/veu_timing_profile_v2_mode_wildcard.csv";
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
    stepUntilResponse(veu, csrWrite(VeuCsr::Config, 3u << 7));
    stepUntilResponse(veu, csrWrite(VeuCsr::VectorLength, 256));
    stepUntilResponse(veu, csrWrite(VeuCsr::Mask, 0xffffffffu));
    stepUntilResponse(veu,
        vectorStart(VeuInstruction::Add, 0x100, 0x200));

    EXPECT_EQ(veu.activeTimingProfileId(), "mode_independent_vadd");
    EXPECT_EQ(veu.activeTimingSource(), "rtl_sim");
    EXPECT_EQ(veu.profileHitCount(), 1u);
    EXPECT_EQ(veu.profileFallbackCount(), 0u);
}

TEST(TimingVeuTest, TimingProfileV2RejectsTimingDimensionWildcard)
{
    TimingVeu veu;
    VeuTimingConfig config;
    config.timingProfilePath =
        "src/brs/veu/testdata/veu_timing_profile_v2_invalid_wildcard.csv";
    EXPECT_THROW(veu.configure(config), std::runtime_error);
}

} // namespace
} // namespace brs
} // namespace gem5
