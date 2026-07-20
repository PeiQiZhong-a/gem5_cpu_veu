#include "brs/pipeline/pipeline_core.hh"

#include <cstdint>
#include <map>

#include <gtest/gtest.h>

namespace gem5
{
namespace
{

std::array<uint8_t, brs::VeuVectorBytes>
makeVector(std::initializer_list<uint32_t> lanes)
{
    std::array<uint8_t, brs::VeuVectorBytes> data = {};
    uint32_t laneIndex = 0;
    for (uint32_t value : lanes) {
        const uint32_t offset = laneIndex * sizeof(uint32_t);
        data[offset] = static_cast<uint8_t>(value & 0xff);
        data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
        data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xff);
        data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xff);
        ++laneIndex;
    }
    return data;
}

uint32_t
readLane(const std::array<uint8_t, brs::VeuVectorBytes> &data,
         uint32_t laneIndex)
{
    const uint32_t offset = laneIndex * sizeof(uint32_t);
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

brs::VeuRequest
csrWrite(brs::VeuCsr csr, uint32_t value)
{
    brs::VeuRequest request;
    request.csrAddr = static_cast<uint16_t>(csr);
    request.csrWrite = true;
    request.writeType = static_cast<uint8_t>(brs::VeuWriteType::Write);
    request.writeData = value;
    return request;
}

void
completeCsrRequest(brs::TimingVeu &veu, const brs::VeuRequest &request)
{
    veu.clock(request);
    ASSERT_TRUE(veu.evaluate().valid);
    veu.clock({});
}

brs::VeuRequest
vaddStart(uint32_t readAddr1, uint32_t readAddr2)
{
    brs::VeuRequest request;
    request.csrAddr = static_cast<uint16_t>(brs::VeuCsr::ReadAddress1);
    request.csrWrite = true;
    request.writeType =
        static_cast<uint8_t>(brs::VeuWriteType::VectorStart);
    request.writeData = brs::packVeuOperands(readAddr1, readAddr2);
    request.veStart = brs::veuStartMask(brs::VeuInstruction::Add);
    return request;
}

TEST(PipelineVeuDmemLockTest, DefersRvLoadUntilVeuStoreCompletes)
{
    PipelineCore core;
    core.reset(0x80000000u, 0x80000000u);
    core.useTimingVeuEndpoint();

    brs::VeuTimingConfig config;
    config.startupCycles = 1;
    config.executeLatency = 4;
    config.finishCycles = 1;
    core.configureTimingVeu(config);

    std::map<uint32_t, std::array<uint8_t, brs::VeuVectorBytes>> memory;
    memory[0x100] = makeVector({1, 2, 3, 4, 5, 6, 7, 8});
    memory[0x200] = makeVector({10, 20, 30, 40, 50, 60, 70, 80});
    memory[0x300] = makeVector({0, 0, 0, 0, 0, 0, 0, 0});

    core.setTimingVeuMemoryRequestCallback(
        [&](const brs::TimingVeuMemoryRequest &request) {
            if (request.isWrite) {
                for (unsigned byte = 0; byte < brs::VeuVectorBytes; ++byte) {
                    if (request.writeStrobe & (uint32_t{1} << byte))
                        memory[request.address][byte] = request.data[byte];
                }
                core.acceptVeuMemoryWrite(request.transactionId);
            } else {
                core.acceptVeuMemoryRead(request.transactionId,
                                         memory[request.address]);
            }
            return true;
        });

    completeCsrRequest(
        core.timingVeu, csrWrite(brs::VeuCsr::WriteAddress, 0x300));
    completeCsrRequest(
        core.timingVeu,
        csrWrite(brs::VeuCsr::VectorLength, brs::VeuVectorBits));
    completeCsrRequest(core.timingVeu, vaddStart(0x100, 0x200));
    ASSERT_TRUE(core.timingVeuOwnsSharedDmem());

    uint64_t rvDataRequests = 0;
    bool rvRequestWhileVeuBusy = false;
    core.requestTimingData =
        [&](uint32_t addr, unsigned size, bool isWrite, uint32_t) {
            ++rvDataRequests;
            rvRequestWhileVeuBusy |= core.timingVeuOwnsSharedDmem();
            EXPECT_EQ(addr, 0x300u);
            EXPECT_EQ(size, sizeof(uint32_t));
            EXPECT_FALSE(isWrite);
            core.acceptDataResponse(addr, readLane(memory[addr], 0), false);
            return true;
        };
    core.fetchInstr = [](uint32_t, uint32_t &) { return false; };

    core.exmem_cur.valid = true;
    core.exmem_cur.pc = 0x80000000u;
    core.exmem_cur.instr = 0x30002403u;
    core.exmem_cur.kind = InstrKind::LW;
    core.exmem_cur.rd = 8;
    core.exmem_cur.alu_result = 0x300u;
    core.exmem_cur.reg_write = true;
    core.exmem_cur.mem_read = true;
    core.exmem_cur.wb_sel = WbSel::MEM;

    for (int cycle = 0;
         cycle < 80 && core.timingVeuOwnsSharedDmem(); ++cycle) {
        core.stepOneCycle();
        EXPECT_EQ(rvDataRequests, 0u);
    }

    ASSERT_FALSE(core.timingVeuOwnsSharedDmem());
    EXPECT_GT(core.getRvDmemBlockedByVeuCycles(), 0u);
    EXPECT_EQ(rvDataRequests, 0u);
    EXPECT_EQ(readLane(memory[0x300], 0), 11u);

    for (int cycle = 0; cycle < 8 && core.getReg(8) != 11u; ++cycle) {
        core.stepOneCycle();
    }

    EXPECT_FALSE(rvRequestWhileVeuBusy);
    EXPECT_EQ(rvDataRequests, 1u);
    EXPECT_EQ(core.getReg(8), 11u);
}

} // namespace
} // namespace gem5
