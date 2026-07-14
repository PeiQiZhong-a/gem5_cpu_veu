#ifndef __BRS_PIPELINE_CORE_HH__
#define __BRS_PIPELINE_CORE_HH__

#include <cstdint>
#include <functional>

#include "brs/pipeline/frontend_fetch_unit.hh"
#include "brs/pipeline/forwarding_unit.hh"
#include "brs/pipeline/hazard_unit.hh"
#include "brs/pipeline/memory_backend_local.hh"
#include "brs/pipeline/pipeline_regs.hh"
#include "brs/pipeline/program_image.hh"
#include "brs/veu/fake_veu.hh"
#include "brs/veu/veu_cbu.hh"
#include "brs/veu/veu_endpoint.hh"

namespace gem5
{

class PipelineCore
{
  public:
    PipelineCore();

    void reset();
    void reset(uint32_t start_pc, uint32_t end_pc);
    void configureFrontend(uint32_t fifoDepth, uint32_t burstBytes);
    void configureFakeVeu(uint32_t latencyCycles, uint32_t responseData);
    void attachVeuEndpoint(brs::VeuEndpoint &endpoint);
    void useFakeVeuEndpoint();
    void stepOneCycle();

    bool done() const;

    uint64_t getCycle() const;
    uint32_t getPC() const;
    uint32_t getReg(int idx) const;
    uint64_t getForwardCount() const;
    uint64_t getRetiredInstCount() const;
    uint64_t getStallCount() const;
    uint64_t getFlushCount() const;
    uint64_t getIbusReqCount() const;
    uint64_t getFetchFifoFlushCount() const;
    uint64_t getAlignedInstrCount() const;
    uint64_t getVeuIssueCount() const { return veu_issue_count; }
    uint64_t getVeuCompleteCount() const { return veu_complete_count; }
    uint64_t getFakeVeuAcceptedRequestCount() const
    {
        return fakeVeu.acceptedRequestCount();
    }
    const brs::VeuRequest &getFakeVeuLastRequest() const
    {
        return fakeVeu.lastAcceptedRequest();
    }
    bool veuBusy() const { return veuCbu.busy(); }
    bool veuStalled() const { return veu_stall; }
    bool mduStalled() const { return mdu_stall; }
    bool spiritExecuteStalled() const { return veu_stall || mdu_stall; }
    bool haltRequested() const { return halt_requested; }

    bool ifidValid() const;
    bool idexValid() const;
    bool exmemValid() const;
    bool memwbValid() const;

    uint32_t regs[32];
    uint32_t pc;
    uint64_t cycle;
    bool done_flag;

    uint64_t forward_count;
    uint64_t retired_inst_count;
    uint64_t stall_count = 0;
    uint64_t flush_count = 0;

    bool stall_pc = false;
    bool stall_ifid = false;
    bool bubble_idex = false;

    bool redirect_pc = false;
    uint32_t redirect_target = 0;
    bool flush_idex = false;

    bool halt_requested = false;

    bool retire_trace_enable = false;
    void traceRetire(uint32_t pc, uint32_t instr,
                     bool has_rd, uint8_t rd, uint32_t data);
    void traceStore(uint32_t pc, uint32_t instr,
                    uint32_t addr, uint32_t data);

    std::function<bool(uint32_t, uint32_t&)> fetchInstr;
    std::function<bool(uint32_t)> requestTimingFetch;
    std::function<bool(uint32_t, unsigned, bool, uint32_t)> requestTimingData;
    uint32_t text_start = 0x80000000u;
    uint32_t text_end = 0x80000000u;
    bool fetch_waiting = false;
    bool fetch_response_valid = false;
    uint32_t fetch_response_addr = 0;
    uint32_t fetch_response_inst = 0;
    bool fetch_block_response_valid = false;
    FetchBlock fetch_block_response;

    bool mem_stall = false;
    bool mem_req_issued = false;
    bool data_waiting = false;
    bool data_response_valid = false;
    uint32_t data_response_addr = 0;
    uint32_t data_response_value = 0;
    bool data_response_is_store = false;

    bool veu_stall = false;
    bool mdu_stall = false;
    bool mdu_busy = false;
    uint32_t mdu_cycles_remaining = 0;
    uint32_t mdu_result = 0;
    brs::VeuCbu veuCbu;
    brs::FakeVeu fakeVeu;
    brs::VeuEndpoint *veuEndpoint = &fakeVeu;
    brs::VeuResponse veuResponse;
    brs::VeuCbuOutput veuCbuOutput;
    brs::VeuCbuIssue veuIssue;
    uint64_t veu_issue_count = 0;
    uint64_t veu_complete_count = 0;

    ProgramImage program;
    LocalMemoryBackend dataMem;

    IFID ifid_cur, ifid_next;
    IDEX idex_cur, idex_next;
    EXMEM exmem_cur, exmem_next;
    MEMWB memwb_cur, memwb_next;

    ForwardingUnit forwardingUnit;
    HazardUnit hazardUnit;
    FrontendFetchUnit frontend;

    static int32_t signExtend12(uint32_t imm12);
    static int32_t signExtend13(uint32_t imm13);
    static int32_t signExtend21(uint32_t imm21);
    bool pipelineEmpty() const;
    void acceptFetchedInst(uint32_t addr, uint32_t inst);
    void acceptFetchedBlock(const FetchBlock &block);
    void acceptDataResponse(uint32_t addr, uint32_t data, bool is_store);

    void stageWB();
    void stageMEM();
    void stageEX();
    void stageID();
    void stageIF();

    void commitNext();
    bool freezeFetchDecodeForExecuteStall() const;
};

} // namespace gem5

#endif
