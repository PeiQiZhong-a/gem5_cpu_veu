#ifndef __BRS_PIPELINE_CORE_HH__
#define __BRS_PIPELINE_CORE_HH__

#include <cstdint>
#include <array>
#include <functional>

#include "brs/pipeline/frontend_fetch_unit.hh"
#include "brs/pipeline/forwarding_unit.hh"
#include "brs/pipeline/hazard_unit.hh"
#include "brs/pipeline/memory_backend_local.hh"
#include "brs/pipeline/pipeline_regs.hh"
#include "brs/pipeline/program_image.hh"
#include "brs/veu/fake_veu.hh"
#include "brs/veu/timing_veu.hh"
#include "brs/veu/veu_cbu.hh"
#include "brs/veu/veu_endpoint.hh"

namespace gem5
{

struct PipelineRetireEvent
{
    bool valid = false;
    uint32_t pc = 0;
    uint32_t instr = 0;
    bool regWrite = false;
    bool fpWrite = false;
    uint8_t rd = 0;
    uint32_t data = 0;
};

class PipelineCore
{
  public:
    PipelineCore();

    void reset();
    void reset(uint32_t start_pc, uint32_t end_pc);
    void configureFrontend(uint32_t fifoDepth, uint32_t burstBytes);
    void configureTextEndTermination(bool enabled);
    void configureTrapVector(uint32_t trapVector);
    void setInterruptInputs(uint32_t external, bool software, bool timer);
    void setDebugInputs(bool halt, bool haltOnReset, bool resume,
                        uint32_t data0 = 0);
    void setDebugInstruction(uint32_t instruction, bool valid);
    void configureFakeVeu(uint32_t latencyCycles, uint32_t responseData);
    void configureTimingVeu(const brs::VeuTimingConfig &config);
    void setTimingVeuMemoryRequestCallback(
        brs::TimingVeu::MemoryRequestFn callback);
    void attachVeuEndpoint(brs::VeuEndpoint &endpoint);
    void useFakeVeuEndpoint();
    void useTimingVeuEndpoint();
    void acceptVeuMemoryRead(uint64_t transactionId,
                             const std::array<uint8_t, brs::VeuVectorBytes> &data);
    void acceptVeuMemoryWrite(uint64_t transactionId);
    void noteVeuMemoryRetry() { timingVeu.noteMemoryRetry(); }
    brs::VeuMemoryOutput evaluateVeuMemory() const;
    void clockVeuMemory(const brs::VeuMemoryResponse &response);
    void stepOneCycle();

    bool done() const;

    uint64_t getCycle() const;
    uint32_t getPC() const;
    uint32_t getReg(int idx) const;
    uint32_t getFpReg(int idx) const;
    uint32_t getCSR(uint16_t addr) const;
    uint64_t getForwardCount() const;
    uint64_t getRetiredInstCount() const;
    uint64_t getStallCount() const;
    uint64_t getFlushCount() const;
    uint64_t getIbusReqCount() const;
    uint64_t getFetchFifoFlushCount() const;
    uint64_t getAlignedInstrCount() const;
    uint64_t getVeuIssueCount() const { return veu_issue_count; }
    uint64_t getVeuCompleteCount() const { return veu_complete_count; }
    uint64_t getVeuCsrHandshakeCycles() const
    {
        return veu_csr_handshake_cycles;
    }
    uint64_t getRvDmemBlockedByVeuCycles() const
    {
        return rv_dmem_blocked_by_veu_cycles;
    }
    uint64_t getTimingVeuOperationStarts() const
    {
        return timingVeu.startedOperationCount();
    }
    uint64_t getTimingVeuOperationCompletes() const
    {
        return timingVeu.completedOperationCount();
    }
    uint64_t getTimingVeuBusyCycles() const
    {
        return timingVeu.busyCycleCount();
    }
    uint64_t getTimingVeuLoadWaitCycles() const
    {
        return timingVeu.loadWaitCycleCount();
    }
    uint64_t getTimingVeuExecuteCycles() const
    {
        return timingVeu.executeCycleCount();
    }
    uint64_t getTimingVeuStoreWaitCycles() const
    {
        return timingVeu.storeWaitCycleCount();
    }
    uint64_t getTimingVeuChunks() const { return timingVeu.chunkCount(); }
    uint64_t getTimingVeuMemoryReads() const
    {
        return timingVeu.memoryReadCount();
    }
    uint64_t getTimingVeuMemoryWrites() const
    {
        return timingVeu.memoryWriteCount();
    }
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
    bool lsuStalled() const { return lsu_stall; }
    bool fpStalled() const { return fp_stall; }
    bool spiritExecuteStalled() const
    {
        return veu_stall || mdu_stall || lsu_stall || fp_stall;
    }
    bool timingVeuOwnsSharedDmem() const
    {
        return veuEndpoint == &timingVeu && timingVeu.lockIsActive();
    }
    bool haltRequested() const { return halt_requested; }
    bool debugMode() const { return csr_debug_mode; }
    uint32_t debugData0() const { return debug_data0; }
    bool debugInstructionReady() const { return debug_instr_ready; }
    bool debugInstructionCaughtEbreak() const
    {
        return debug_instr_caught_ebreak;
    }
    const PipelineRetireEvent &lastRetireEvent() const
    {
        return last_retire_event;
    }

    bool ifidValid() const;
    bool idexValid() const;
    bool exmemValid() const;
    bool memwbValid() const;

    uint32_t regs[32];
    uint32_t fp_regs[32];
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

    // CSRU state.  The reset values and writable masks mirror CSRU.sv,
    // including Spirit's internal exception encodings.
    bool csr_machine_mode = true;
    bool csr_mstatus_mpie = false;
    bool csr_mstatus_mie = false;
    bool csr_mstatus_mpp = true;
    bool csr_mstatus_mprv = false;
    bool csr_mstatus_tw = false;
    uint32_t csr_mscratch = 0;
    uint32_t csr_mtvec = 0x100;
    uint32_t csr_mepc = 0;
    uint32_t csr_mie = 0;
    bool csr_mcountinhibit_cycle = true;
    bool csr_mcountinhibit_instr = true;
    uint64_t csr_mcycle = 0;
    uint64_t csr_minstret = 0;
    uint8_t csr_fflags = 0;
    uint8_t csr_frm = 0;
    uint8_t csr_mcounteren = 0x5;
    uint32_t csr_external_enable = 0;
    uint32_t csr_external_force = 0;
    uint8_t csr_external_preempt = 0;
    bool csr_external_noirq = true;
    uint8_t csr_external_irq = 0;
    bool csr_external_mreteirq = false;
    uint8_t csr_msleep = 0;
    bool csr_tcontrol_mte = false;
    bool csr_tcontrol_mpte = false;
    bool csr_debug_mode = false;
    bool csr_dcsr_ebreakm = false;
    bool csr_dcsr_ebreaku = false;
    bool csr_dcsr_step = false;
    uint8_t csr_dcsr_cause = 0;
    uint32_t csr_dpc = 0;

    uint32_t trap_vector_reset = 0x100;
    uint32_t irq_external_input = 0;
    bool irq_software_input = false;
    bool irq_timer_input = false;
    uint32_t irq_external_sampled = 0;
    bool irq_software_sampled = false;
    bool irq_timer_sampled = false;
    bool debug_halt_input = false;
    bool debug_halt_on_reset_input = false;
    bool debug_resume_input = false;
    uint32_t debug_data0_input = 0;
    bool debug_halt_sampled = false;
    bool debug_halt_on_reset_latched = false;
    bool debug_resume_sampled = false;
    bool debug_step_halt_requested = false;
    bool debug_have_just_reset = true;
    uint32_t debug_data0 = 0;
    uint32_t debug_instr_input = 0;
    bool debug_instr_valid = false;
    bool debug_instr_ready = false;
    bool debug_instr_caught_ebreak = false;

    bool retire_trace_enable = false;
    PipelineRetireEvent last_retire_event;
    void traceRetire(uint32_t pc, uint32_t instr,
                     bool has_rd, uint8_t rd, uint32_t data);
    void traceStore(uint32_t pc, uint32_t instr,
                    uint32_t addr, uint32_t data);

    std::function<bool(uint32_t, uint32_t&)> fetchInstr;
    std::function<bool(uint32_t)> requestTimingFetch;
    std::function<bool(uint32_t, unsigned, bool, uint32_t)> requestTimingData;
    uint32_t text_start = 0x80000000u;
    uint32_t text_end = 0x80000000u;
    bool text_end_termination = true;
    bool fetch_waiting = false;
    bool fetch_response_valid = false;
    uint32_t fetch_response_addr = 0;
    uint32_t fetch_response_inst = 0;
    bool fetch_block_response_valid = false;
    FetchBlock fetch_block_response;

    bool mem_req_issued = false;
    uint32_t mem_request_addr = 0;
    uint32_t mem_request_store_data = 0;
    bool data_waiting = false;
    bool data_response_valid = false;
    uint32_t data_response_addr = 0;
    uint32_t data_response_value = 0;
    bool data_response_is_store = false;

    bool veu_stall = false;
    bool mdu_stall = false;
    bool lsu_stall = false;
    bool fp_stall = false;
    bool mdu_busy = false;
    uint32_t mdu_cycles_remaining = 0;
    uint32_t mdu_result = 0;
    brs::VeuCbu veuCbu;
    brs::FakeVeu fakeVeu;
    brs::TimingVeu timingVeu;
    brs::VeuEndpoint *veuEndpoint = &fakeVeu;
    brs::VeuResponse veuResponse;
    brs::VeuCbuOutput veuCbuOutput;
    brs::VeuCbuIssue veuIssue;
    uint64_t veu_issue_count = 0;
    uint64_t veu_complete_count = 0;
    uint64_t veu_csr_handshake_cycles = 0;
    uint64_t rv_dmem_blocked_by_veu_cycles = 0;

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

    uint32_t readCsr(uint16_t addr, uint32_t rawOperand = 0) const;
    void writeCsr(uint16_t addr, uint32_t rawOperand,
                  CsrWriteType writeType);
    void applyCsrReadSideEffects(uint16_t addr, uint32_t rawOperand);
    uint32_t decodeForwardedReg(uint8_t reg, bool fp = false) const;
    uint32_t externalPendingEligible() const;
    uint8_t externalWinner() const;
    bool interruptActive() const;
    uint8_t interruptCode() const;
    void enterTrap(uint32_t mepc, uint8_t cause, bool interrupt);
    void returnFromTrap();
    void enterDebug(uint32_t dpc, uint8_t cause);

    void commitNext();
    bool freezeFetchDecodeForExecuteStall() const;
};

} // namespace gem5

#endif
