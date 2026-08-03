#include "brs/pipeline/pipeline_core.hh"

#include <cstdint>
#include <map>

#include <gtest/gtest.h>

namespace gem5
{
namespace
{

constexpr uint32_t
encodeAddi(uint8_t rd, uint8_t rs1, uint16_t imm)
{
    return ((static_cast<uint32_t>(imm) & 0xfff) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(rd) << 7) |
           0x13u;
}

std::array<uint8_t, brs::VeuVectorBytes>
makeVector(std::initializer_list<uint32_t> lanes)
{
    std::array<uint8_t, brs::VeuVectorBytes> data = {};
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
lane(const std::array<uint8_t, brs::VeuVectorBytes> &data, uint32_t index)
{
    const uint32_t offset = index * sizeof(uint32_t);
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

IDEX
makeVectorAddInExecute()
{
    IDEX idex;
    idex.valid = true;
    idex.pc = 0x80000000u;
    idex.instr = 0x002083ebu;
    idex.kind = InstrKind::VEU;
    idex.rd = 7;
    idex.rs1 = 1;
    idex.rs2 = 2;
    idex.rs1_val = 0x100u;
    idex.rs2_val = 0x200u;
    idex.veu_operation = brs::VeuInstruction::Add;
    idex.veu_csr_addr =
        static_cast<uint16_t>(brs::VeuCsr::ReadAddress1);
    idex.veu_csr_read = true;
    idex.veu_csr_write = true;
    idex.veu_write_type = brs::VeuWriteType::VectorStart;
    idex.veu_start = brs::veuStartMask(brs::VeuInstruction::Add);
    idex.reg_write = true;
    idex.wb_sel = WbSel::VEU;
    return idex;
}

TEST(PipelineVeuAsyncTest, ReleasesRvAfterCsrHandshakeWhileOperationRuns)
{
    PipelineCore core;
    core.reset(0x80000000u, 0x80000000u);
    core.useTimingVeuEndpoint();

    brs::VeuTimingConfig config;
    config.startupCycles = 2;
    config.executeLatency = 4;
    core.configureTimingVeu(config);

    std::map<uint32_t, std::array<uint8_t, brs::VeuVectorBytes>> memory;
    memory[0x100] = makeVector({1, 2, 3, 4, 5, 6, 7, 8});
    memory[0x200] = makeVector({10, 20, 30, 40, 50, 60, 70, 80});
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
    completeCsrRequest(
        core.timingVeu, csrWrite(brs::VeuCsr::Mask, 0xffffffffu));

    core.fetchInstr = [](uint32_t, uint32_t &) { return false; };
    core.idex_cur = makeVectorAddInExecute();
    core.ifid_cur.valid = true;
    core.ifid_cur.pc = 0x80000004u;
    core.ifid_cur.instr = encodeAddi(5, 0, 7);
    core.ifid_cur.instr_len = 4;

    for (int i = 0; i < 12 && core.getVeuCompleteCount() == 0; ++i) {
        core.stepOneCycle();
    }

    ASSERT_EQ(core.getVeuCompleteCount(), 1u);
    EXPECT_GT(core.getVeuCsrHandshakeCycles(), 0u);
    EXPECT_TRUE(core.timingVeu.operationBusy());
    EXPECT_EQ(core.getTimingVeuOperationStarts(), 1u);
    EXPECT_EQ(core.getTimingVeuOperationCompletes(), 0u);
    EXPECT_FALSE(core.spiritExecuteStalled());
    ASSERT_TRUE(core.idex_cur.valid);
    EXPECT_EQ(core.idex_cur.kind, InstrKind::ADDI);

    bool sawDrainedRvPipelineWhileVeuBusy = false;
    for (int i = 0;
         i < 40 && core.getTimingVeuOperationCompletes() == 0; ++i) {
        core.stepOneCycle();
        const bool rvPipelineDrained =
            !core.ifidValid() && !core.idexValid() &&
            !core.exmemValid() && !core.memwbValid();
        if (rvPipelineDrained && core.timingVeu.operationBusy()) {
            sawDrainedRvPipelineWhileVeuBusy = true;
            EXPECT_FALSE(core.done());
        }
    }

    EXPECT_TRUE(sawDrainedRvPipelineWhileVeuBusy);
    EXPECT_EQ(core.getTimingVeuOperationCompletes(), 1u);
    EXPECT_FALSE(core.timingVeu.operationBusy());
    EXPECT_TRUE(core.done());
    EXPECT_EQ(lane(memory[0x300], 0), 11u);
    EXPECT_EQ(lane(memory[0x300], 7), 88u);
}

} // namespace
} // namespace gem5
