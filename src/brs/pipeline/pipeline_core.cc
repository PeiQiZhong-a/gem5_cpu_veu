#include "brs/pipeline/pipeline_core.hh"
#include "brs/pipeline/forwarding_unit.hh"
#include "brs/pipeline/hazard_unit.hh"
#include "brs/pipeline/memory_backend_local.hh"

#include <cstring>
#include <iostream>
#include <cstdlib>
#include <iomanip>
#include <utility>



namespace gem5
{

PipelineCore::PipelineCore()
{
    reset();
}

void
PipelineCore::configureFrontend(uint32_t fifoDepth, uint32_t burstBytes)
{
    frontend.configure({fifoDepth, burstBytes});
}

void
PipelineCore::configureTextEndTermination(bool enabled)
{
    text_end_termination = enabled;
}

void
PipelineCore::configureTrapVector(uint32_t trapVector)
{
    trap_vector_reset = trapVector;
    csr_mtvec = trapVector;
}

void
PipelineCore::setInterruptInputs(
    uint32_t external, bool software, bool timer)
{
    irq_external_input = external;
    irq_software_input = software;
    irq_timer_input = timer;
}

void
PipelineCore::setDebugInputs(
    bool halt, bool haltOnReset, bool resume, uint32_t data0)
{
    debug_halt_input = halt;
    debug_halt_on_reset_input = haltOnReset;
    debug_resume_input = resume;
    debug_data0_input = data0;
}

void
PipelineCore::setDebugInstruction(uint32_t instruction, bool valid)
{
    debug_instr_input = instruction;
    debug_instr_valid = valid;
}

void
PipelineCore::configureFakeVeu(
    uint32_t latencyCycles, uint32_t responseData)
{
    fakeVeu.setResponseLatencyCycles(latencyCycles);
    fakeVeu.setResponseData(responseData);
    useFakeVeuEndpoint();
}

void
PipelineCore::configureStubSau(uint32_t latencyCycles)
{
    stubSau.setResponseLatencyCycles(latencyCycles);
    useStubSauEndpoint();
}

void
PipelineCore::attachSauEndpoint(brs::SauEndpoint &endpoint)
{
    sauEndpoint = &endpoint;
    sauEndpoint->reset();
}

void
PipelineCore::useStubSauEndpoint()
{
    sauEndpoint = &stubSau;
    sauEndpoint->reset();
}

void
PipelineCore::configureTimingVeu(const brs::VeuTimingConfig &config)
{
    timingVeu.configure(config);
}

void
PipelineCore::setTimingVeuMemoryRequestCallback(
    brs::TimingVeu::MemoryRequestFn callback)
{
    timingVeu.setMemoryRequestCallback(std::move(callback));
}

void
PipelineCore::attachVeuEndpoint(brs::VeuEndpoint &endpoint)
{
    veuEndpoint = &endpoint;
    veuEndpoint->reset();
}

void
PipelineCore::useFakeVeuEndpoint()
{
    veuEndpoint = &fakeVeu;
    veuEndpoint->reset();
}

void
PipelineCore::useTimingVeuEndpoint()
{
    veuEndpoint = &timingVeu;
    veuEndpoint->reset();
}

void
PipelineCore::acceptVeuMemoryRead(
    uint64_t transactionId,
    const std::array<uint8_t, brs::VeuVectorBytes> &data)
{
    timingVeu.completeMemoryRead(transactionId, data);
}

void
PipelineCore::acceptVeuMemoryWrite(uint64_t transactionId)
{
    timingVeu.completeMemoryWrite(transactionId);
}

void
PipelineCore::reset()
{
    std::memset(regs, 0, sizeof(regs));
    std::memset(fp_regs, 0, sizeof(fp_regs));
    pc = text_start;
    cycle = 0;
    forward_count = 0;       //stats
    retired_inst_count = 0;  //stats
    stall_count = 0;         //stats
    flush_count = 0;
    halt_requested = false;
    done_flag = false;

    csr_machine_mode = true;
    csr_mstatus_mpie = false;
    csr_mstatus_mie = false;
    csr_mstatus_mpp = true;
    csr_mstatus_mprv = false;
    csr_mstatus_tw = false;
    csr_mscratch = 0;
    csr_mtvec = trap_vector_reset;
    csr_mepc = 0;
    csr_mie = 0;
    csr_mcountinhibit_cycle = true;
    csr_mcountinhibit_instr = true;
    csr_mcycle = 0;
    csr_minstret = 0;
    csr_fflags = 0;
    csr_frm = 0;
    csr_mcounteren = 0x5;
    csr_external_enable = 0;
    csr_external_force = 0;
    csr_external_preempt = 0;
    csr_external_noirq = true;
    csr_external_irq = 0;
    csr_external_mreteirq = false;
    csr_msleep = 0;
    csr_tcontrol_mte = false;
    csr_tcontrol_mpte = false;
    csr_debug_mode = false;
    csr_dcsr_ebreakm = false;
    csr_dcsr_ebreaku = false;
    csr_dcsr_step = false;
    csr_dcsr_cause = 0;
    csr_dpc = 0;
    irq_external_sampled = 0;
    irq_software_sampled = false;
    irq_timer_sampled = false;
    debug_halt_sampled = false;
    debug_halt_on_reset_latched = false;
    debug_resume_sampled = false;
    debug_step_halt_requested = false;
    debug_have_just_reset = true;
    debug_data0 = 0;
    debug_instr_ready = false;
    debug_instr_caught_ebreak = false;
    fetch_waiting = false;
    fetch_response_valid = false;
    fetch_response_addr = 0;
    fetch_response_inst = 0;
    fetch_block_response_valid = false;
    fetch_block_response = {};
    mem_req_issued = false;
    mem_request_addr = 0;
    mem_request_store_data = 0;
    data_waiting = false;
    data_response_valid = false;
    data_response_addr = 0;
    data_response_value = 0;
    data_response_is_store = false;
    veu_stall = false;
    sau_stall = false;
    mdu_stall = false;
    lsu_stall = false;
    fp_stall = false;
    predict_failed = false;
    mdu_busy = false;
    mdu_cycles_remaining = 0;
    mdu_result = 0;
    hcResponse = {};
    hcCbuOutput = {};
    hcIssue = {};
    hcRoutedRequests = {};
    sauMemoryResponse = {};
    veuMemoryResponse = {};
    cycleEvaluated = false;
    instrRetiringThisCycle = false;
    sauResponse = {};
    sau_issue_count = 0;
    sau_complete_count = 0;
    sau_csr_handshake_cycles = 0;
    hcCbu.reset();
    if (sauEndpoint) {
        sauEndpoint->reset();
    }
    veuResponse = {};
    veu_issue_count = 0;
    veu_complete_count = 0;
    veu_csr_handshake_cycles = 0;
    rv_dmem_blocked_by_veu_cycles = 0;
    if (veuEndpoint) {
        veuEndpoint->reset();
    }
    frontend.reset(pc);

    redirect_pc = false;
    redirect_target = 0;
    jcu_redirect_pending = false;
    jcu_redirect_target_pending = 0;
    flush_idex = false;
        

    ifid_cur = {};
    ifid_next = {};
    idex_cur = {};
    idex_next = {};
    exmem_cur = {};
    exmem_next = {};
    memwb_cur = {};
    memwb_next = {};

    program = {};

    const char *trace_env = std::getenv("BRS_RETIRE_TRACE");
    retire_trace_enable =  (trace_env && *trace_env && std::string(trace_env) != "0");
    last_retire_event = {};


    const char *hex_path = std::getenv("BRS_PROGRAM_HEX");

      if (hex_path && *hex_path) {
          if (!program.loadHexFile(hex_path)) {
              std::cerr << "Failed to load program hex: "
                        << hex_path << std::endl;
              std::abort();
          }
      } else {
          //program.loadAddiTest();
            //program.loadBeqFlushTakenTest();
            //program.loadBeqNotTakenTest();
            //program.loadJalTest();
            //program.loadLuiBasicTest();
            //program.loadAuipcBasicTest();
            program.loadOriBasicTest();
            //program.loadAndiBasicTest();
            //program.loadXoriBasicTest();
            //program.loadSltiBasicTest();
            //program.loadSltiuBasicTest();
            //program.loadOrBasicTest();
            //program.loadAndBasicTest();
            //program.loadXorBasicTest();
            //program.loadSltBasicTest();
            //program.loadSltuBasicTest();
      }

}

void
PipelineCore::reset(uint32_t start_pc, uint32_t end_pc)
{
    text_start = start_pc;
    text_end = end_pc;
    reset();
}



int32_t
PipelineCore::signExtend12(uint32_t imm12)
{
    if (imm12 & 0x800) {
        return static_cast<int32_t>(imm12 | 0xFFFFF000u);
    }
    return static_cast<int32_t>(imm12);
}
int32_t
PipelineCore::signExtend13(uint32_t imm13)
{
    if (imm13 & 0x1000) {
        return static_cast<int32_t>(imm13 | 0xFFFFE000u);
    }
    return static_cast<int32_t>(imm13);
}
int32_t
PipelineCore::signExtend21(uint32_t imm21)
{
    if (imm21 & 0x100000) {
        return static_cast<int32_t>(imm21 | 0xFFE00000u);
    }
    return static_cast<int32_t>(imm21);
}

//统计
uint64_t
PipelineCore::getForwardCount() const
{
    return forward_count;
}
uint64_t
PipelineCore::getRetiredInstCount() const
{
    return retired_inst_count;
}
uint64_t
PipelineCore::getStallCount() const
{
    return stall_count;
}
uint64_t
PipelineCore::getFlushCount() const
{
    return flush_count;
}

uint64_t
PipelineCore::getIbusReqCount() const
{
    return frontend.getIbusReqCount();
}

uint64_t
PipelineCore::getFetchFifoFlushCount() const
{
    return frontend.getFifoFlushCount();
}

uint64_t
PipelineCore::getAlignedInstrCount() const
{
    return frontend.getAlignedInstrCount();
}


bool
PipelineCore::pipelineEmpty() const
{
    return !ifid_cur.valid && !idex_cur.valid &&
           !exmem_cur.valid && !memwb_cur.valid;
}

void
PipelineCore::acceptFetchedInst(uint32_t addr, uint32_t inst)
{
    fetch_waiting = false;
    fetch_response_valid = true;
    fetch_response_addr = addr;
    fetch_response_inst = inst;
}

void
PipelineCore::acceptFetchedBlock(const FetchBlock &block)
{
    fetch_waiting = false;
    fetch_block_response_valid = true;
    fetch_block_response = block;
}

void
PipelineCore::commitNext()
{
    if (!freezeFetchDecodeForExecuteStall()) {
        ifid_cur = ifid_next;
        idex_cur = idex_next;
    }
    exmem_cur = exmem_next;
    memwb_cur = memwb_next;
}

bool
PipelineCore::freezeFetchDecodeForExecuteStall() const
{
    // Spirit RTL path:
    //   IEU: long-latency unit not ready -> ie2sc.stall_req
    //   SCU: stall_from_ie               -> stall = b111
    //   IFU/IDU: stall bits hold fetch/decode state while EX waits.
    //
    // This software pipeline models that execute-stage freeze here:
    // the current long-latency instruction remains in ID/EX, IF/ID is preserved, and
    // no younger instruction is decoded/fetched until the CBU/MDU/LSU
    // completes. EX/MEM and MEM/WB still advance so older work can retire.
    return veu_stall || sau_stall || mdu_stall || lsu_stall || fp_stall;
}

uint32_t
PipelineCore::readCsr(uint16_t addr, uint32_t rawOperand) const
{
    switch (addr) {
      case 0x001:
        return csr_fflags;
      case 0x002:
        return csr_frm;
      case 0x003:
        return (static_cast<uint32_t>(csr_frm) << 5) | csr_fflags;
      case 0x300:
        return (static_cast<uint32_t>(csr_mstatus_tw) << 21) |
               (static_cast<uint32_t>(csr_mstatus_mprv) << 17) |
               (static_cast<uint32_t>(csr_mstatus_mpp) * 3u << 11) |
               (static_cast<uint32_t>(csr_mstatus_mpie) << 7) |
               (static_cast<uint32_t>(csr_mstatus_mie) << 3);
      case 0x301:
        return 0x40901104u;
      case 0x304:
        return csr_mie;
      case 0x305:
        return (csr_mtvec & ~0x3u) | (csr_mtvec & 0x1u);
      case 0x306:
        return csr_mcounteren;
      case 0x320:
        return (static_cast<uint32_t>(csr_mcountinhibit_instr) << 2) |
               static_cast<uint32_t>(csr_mcountinhibit_cycle);
      case 0x340:
        return csr_mscratch;
      case 0x341:
        return csr_mepc;
      case 0x342:
        // CSRU.sv does not retain mcause.  Outside the entry cycle it reads
        // as an interrupt-tagged zero; preserve that generated-RTL behavior.
        return 0x80000000u |
               (interruptActive() ? interruptCode() : 0u);
      case 0x343:
      case 0x344:
        return 0;
      case 0xB00:
      case 0xC00:
        return static_cast<uint32_t>(csr_mcycle);
      case 0xB80:
      case 0xC80:
        return static_cast<uint32_t>(csr_mcycle >> 32);
      case 0xB02:
      case 0xC02:
        return static_cast<uint32_t>(csr_minstret);
      case 0xB82:
      case 0xC82:
        return static_cast<uint32_t>(csr_minstret >> 32);
      case 0xBE0:
      {
        const unsigned bank = rawOperand & 1u;
        return ((csr_external_enable >> (bank * 16)) & 0xffffu) << 16;
      }
      case 0xBE1:
      {
        const unsigned bank = rawOperand & 1u;
        const uint32_t pending = irq_external_sampled | csr_external_force;
        return ((pending >> (bank * 16)) & 0xffffu) << 16;
      }
      case 0xBE2:
      {
        const unsigned bank = rawOperand & 1u;
        return ((csr_external_force >> (bank * 16)) & 0xffffu) << 16;
      }
      case 0xBE3:
        // The generated priority array is tied to zero.
        return 0;
      case 0xBE4:
      {
        const uint32_t pending = externalPendingEligible();
        const bool noirq = pending == 0;
        return (static_cast<uint32_t>(noirq) << 31) |
               (static_cast<uint32_t>(externalWinner()) << 2);
      }
      case 0xBE5:
        return (static_cast<uint32_t>(csr_external_preempt & 0x10u) << 16) |
               (static_cast<uint32_t>(csr_external_noirq) << 15) |
               (static_cast<uint32_t>(csr_external_irq & 0x1fu) << 4) |
               static_cast<uint32_t>(csr_external_mreteirq);
      case 0xBF0:
        return csr_msleep & 0x7u;
      case 0x7A5:
        return (static_cast<uint32_t>(csr_tcontrol_mte) << 7) |
               (static_cast<uint32_t>(csr_tcontrol_mpte) << 3);
      case 0x7B0:
        return 0x40000000u |
               (static_cast<uint32_t>(csr_dcsr_ebreakm) << 15) |
               (static_cast<uint32_t>(csr_dcsr_ebreaku) << 12) |
               (3u << 9) |
               (static_cast<uint32_t>(csr_dcsr_cause & 0x7u) << 6) |
               (static_cast<uint32_t>(csr_dcsr_step) << 2) |
               (csr_machine_mode ? 3u : 0u);
      case 0x7B1:
        return csr_dpc;
      case 0xBFF:
        return debug_data0_input;
      case 0xF11:
        return 0xace1ab00u;
      case 0xF12:
        return 0x1bu;
      case 0xF13:
        return 0xace1ab01u;
      case 0xF14:
        return 0xace1ab02u;
      case 0xF15:
        return 0xace1ab03u;
      default:
        return 0;
    }
}

void
PipelineCore::writeCsr(
    uint16_t addr, uint32_t rawOperand, CsrWriteType writeType)
{
    // CSRU's w_we_machine_mode gates all implemented CSR writes.  Debug CSRs
    // are further restricted in their individual cases below.
    if (!csr_machine_mode && !csr_debug_mode) {
        return;
    }

    const uint32_t oldValue = readCsr(addr, rawOperand);
    uint32_t value = rawOperand;
    if (writeType == CsrWriteType::SET) {
        value = oldValue | rawOperand;
    } else if (writeType == CsrWriteType::CLEAR) {
        value = oldValue & ~rawOperand;
    }

    switch (addr) {
      case 0x001:
        csr_fflags = value & 0x1f;
        break;
      case 0x002:
        csr_frm = value & 0x7;
        break;
      case 0x003:
        csr_fflags = value & 0x1f;
        csr_frm = (value >> 5) & 0x7;
        break;
      case 0x300:
        csr_mstatus_mie = (value >> 3) & 1;
        csr_mstatus_mpie = (value >> 7) & 1;
        csr_mstatus_mpp = (value >> 12) & 1;
        csr_mstatus_mprv = (value >> 17) & 1;
        csr_mstatus_tw = (value >> 21) & 1;
        break;
      case 0x304:
        csr_mie = (csr_mie & 0xfffff777u) | (value & 0x888u);
        break;
      case 0x305:
        csr_mtvec = (value & 0xfffffffdu) | (csr_mtvec & 0x2u);
        break;
      case 0x306:
        csr_mcounteren = value & 0x7;
        break;
      case 0x320:
        // These assignments intentionally follow CSRU.sv lines 421-423.
        csr_mcountinhibit_cycle = (value >> 2) & 1;
        csr_mcountinhibit_instr = value & 1;
        break;
      case 0x340:
        csr_mscratch = value;
        break;
      case 0x341:
        csr_mepc = value & 0xfffffffeu;
        break;
      case 0xB00:
        csr_mcycle = (csr_mcycle & 0xffffffff00000000ull) | value;
        break;
      case 0xB80:
        csr_mcycle = (csr_mcycle & 0xffffffffull) |
                     (static_cast<uint64_t>(value) << 32);
        break;
      case 0xB02:
        csr_minstret = (csr_minstret & 0xffffffff00000000ull) | value;
        break;
      case 0xB82:
        csr_minstret = (csr_minstret & 0xffffffffull) |
                       (static_cast<uint64_t>(value) << 32);
        break;
      case 0xBE0:
      {
        const unsigned bank = rawOperand & 1u;
        const uint32_t mask = 0xffffu << (bank * 16);
        csr_external_enable =
            (csr_external_enable & ~mask) |
            (((value >> 16) & 0xffffu) << (bank * 16));
        break;
      }
      case 0xBE2:
      {
        const unsigned bank = rawOperand & 1u;
        const uint32_t mask = 0xffffu << (bank * 16);
        csr_external_force =
            (csr_external_force & ~mask) |
            (((value >> 16) & 0xffffu) << (bank * 16));
        break;
      }
      case 0xBE4:
        if ((value & 1u) != 0) {
            csr_external_preempt = 0x10;
            csr_external_noirq = externalPendingEligible() == 0;
            csr_external_irq = externalWinner();
        }
        break;
      case 0xBE5:
      {
        const bool clearContext = writeType != CsrWriteType::CLEAR &&
                                  (rawOperand & 2u);
        if (clearContext) {
            csr_external_preempt = 0;
            csr_external_noirq = true;
            csr_external_irq = 0;
            csr_external_mreteirq = false;
        } else {
            csr_external_preempt = (value & (1u << 20)) ? 0x10 : 0;
            csr_external_noirq = (value >> 15) & 1u;
            csr_external_irq = (value >> 4) & 0x1fu;
            csr_external_mreteirq = value & 1u;
            if ((value & (1u << 2)) != 0) {
                csr_mie |= 1u << 3;
            }
        }
        break;
      }
      case 0xBF0:
        if (csr_machine_mode || csr_debug_mode) {
            csr_msleep = value & 0x7u;
        }
        break;
      case 0x7A5:
        if (csr_machine_mode || csr_debug_mode) {
            csr_tcontrol_mte = (value >> 7) & 1u;
            csr_tcontrol_mpte = (value >> 3) & 1u;
        }
        break;
      case 0x7B0:
        if (csr_debug_mode) {
            csr_dcsr_ebreakm = (value >> 15) & 1u;
            csr_dcsr_ebreaku = (value >> 12) & 1u;
            csr_dcsr_step = (value >> 2) & 1u;
            csr_machine_mode = (value & 0x3u) != 0;
        }
        break;
      case 0x7B1:
        if (csr_debug_mode) {
            csr_dpc = value & 0xfffffffeu;
        }
        break;
      case 0xBFF:
        if (csr_debug_mode) {
            debug_data0 = value;
        }
        break;
      default:
        break;
    }
}

void
PipelineCore::applyCsrReadSideEffects(uint16_t addr, uint32_t)
{
    if (addr != 0xBE4 || !(csr_machine_mode || csr_debug_mode)) {
        return;
    }
    const uint32_t pending = externalPendingEligible();
    if (pending != 0) {
        csr_external_force &= ~(uint32_t{1} << externalWinner());
    }
}

uint32_t
PipelineCore::decodeForwardedReg(uint8_t reg, bool fp) const
{
    if (!fp && reg == 0) {
        return 0;
    }

    const auto exmemValue = [](const EXMEM &entry) {
        return entry.wb_sel == WbSel::MEM ? entry.mem_data :
            entry.alu_result;
    };
    const auto memwbValue = [](const MEMWB &entry) {
        return entry.wb_sel == WbSel::MEM ? entry.mem_data :
            entry.alu_result;
    };

    if (exmem_next.valid && exmem_next.reg_write &&
        exmem_next.rd_fp == fp && exmem_next.rd == reg) {
        return exmemValue(exmem_next);
    }
    if (exmem_cur.valid && exmem_cur.reg_write &&
        exmem_cur.rd_fp == fp && exmem_cur.rd == reg) {
        return exmemValue(exmem_cur);
    }
    if (memwb_next.valid && memwb_next.reg_write &&
        memwb_next.rd_fp == fp && memwb_next.rd == reg) {
        return memwbValue(memwb_next);
    }
    return fp ? fp_regs[reg] : regs[reg];
}

uint32_t
PipelineCore::externalPendingEligible() const
{
    return (irq_external_sampled | csr_external_force) &
           csr_external_enable;
}

uint8_t
PipelineCore::externalWinner() const
{
    const uint32_t pending = externalPendingEligible();
    for (uint8_t bit = 0; bit < 32; ++bit) {
        if (pending & (uint32_t{1} << bit)) {
            return bit;
        }
    }
    return 0;
}

bool
PipelineCore::interruptActive() const
{
    const bool external =
        csr_external_preempt == 0 && externalPendingEligible() != 0 &&
        (csr_mie & (1u << 11));
    const bool software = irq_software_sampled && (csr_mie & (1u << 3));
    const bool timer = irq_timer_sampled && (csr_mie & (1u << 7));
    return (external || software || timer) &&
           (csr_mstatus_mie || !csr_machine_mode) && !csr_dcsr_step;
}

uint8_t
PipelineCore::interruptCode() const
{
    if (csr_external_preempt == 0 && externalPendingEligible() != 0 &&
        (csr_mie & (1u << 11))) {
        return 11;
    }
    if (irq_software_sampled && (csr_mie & (1u << 3))) {
        return 3;
    }
    return irq_timer_sampled && (csr_mie & (1u << 7)) ? 7 : 0;
}

void
PipelineCore::enterTrap(uint32_t mepc, uint8_t cause, bool interrupt)
{
    csr_mepc = mepc & 0xfffffffeu;
    csr_mstatus_mpp = csr_machine_mode;
    csr_mstatus_mpie = csr_mstatus_mie;
    csr_mstatus_mie = false;
    csr_machine_mode = true;

    csr_tcontrol_mpte = csr_tcontrol_mte;
    csr_tcontrol_mte = false;
    if (interrupt && cause == 11) {
        csr_external_preempt = 0x10;
        csr_external_mreteirq = true;
    }

    redirect_pc = true;
    redirect_target = (csr_mtvec & ~0x3u) |
        ((interrupt && (csr_mtvec & 1u)) ?
            (static_cast<uint32_t>(cause) << 2) : 0u);
}

void
PipelineCore::returnFromTrap()
{
    csr_machine_mode = csr_mstatus_mpp;
    csr_mstatus_mie = csr_mstatus_mpie;
    csr_mstatus_mpie = true;
    csr_mstatus_mprv = csr_mstatus_mpp && csr_mstatus_mprv;
    csr_tcontrol_mte = csr_tcontrol_mpte;
    if (csr_external_mreteirq) {
        csr_external_preempt = 0;
        csr_external_mreteirq = false;
    }
    redirect_pc = true;
    redirect_target = csr_mepc;
}

void
PipelineCore::enterDebug(uint32_t dpc, uint8_t cause)
{
    csr_debug_mode = true;
    csr_dpc = dpc & 0xfffffffeu;
    csr_dcsr_cause = cause & 0x7u;
    debug_halt_on_reset_latched = false;
    debug_step_halt_requested = false;
    halt_requested = true;
    ifid_next = {};
}

brs::VeuMemoryOutput
PipelineCore::evaluateVeuMemory() const
{
    return veuEndpoint ? veuEndpoint->evaluateMemory() :
        brs::VeuMemoryOutput{};
}

void
PipelineCore::clockVeuMemory(const brs::VeuMemoryResponse &response)
{
    veuMemoryResponse = response;
}

brs::SauMemoryOutput
PipelineCore::evaluateSauMemory() const
{
    return sauEndpoint ? sauEndpoint->evaluateMemory() :
        brs::SauMemoryOutput{};
}

void
PipelineCore::clockSauMemory(const brs::SauMemoryResponse &response)
{
    sauMemoryResponse = response;
}

void
PipelineCore::evaluateOneCycle()
{
    if (done_flag || cycleEvaluated) {
        return;
    }
    cycleEvaluated = true;

    instrRetiringThisCycle = memwb_cur.valid;
    last_retire_event = {};

    // JCU redirect is registered in Spirit.  Promote the decision made by ID
    // on the previous edge before evaluating this edge's pipeline stages.
    redirect_pc = jcu_redirect_pending;
    redirect_target = jcu_redirect_pending ?
        jcu_redirect_target_pending : 0;
    predict_failed = jcu_redirect_pending;
    jcu_redirect_pending = false;
    jcu_redirect_target_pending = 0;
    flush_idex = false;
    veu_stall = false;
    sau_stall = false;
    mdu_stall = false;
    lsu_stall = false;
    fp_stall = false;
    debug_instr_caught_ebreak = false;
    hcIssue = {};
    sauResponse = sauEndpoint ? sauEndpoint->evaluate() : brs::SauResponse{};
    veuResponse = veuEndpoint ? veuEndpoint->evaluate() : brs::VeuResponse{};
    const brs::HcCbuOutput hcProbe = hcCbu.evaluate({});
    hcResponse = hcRouter.routeResponse(
        hcProbe.request, veuResponse, sauResponse);
    hcCbuOutput = hcCbu.evaluate(hcResponse);
    hcRoutedRequests = hcRouter.routeRequest(hcCbuOutput.request);

    if (csr_debug_mode && debug_resume_sampled) {
        csr_debug_mode = false;
        halt_requested = false;
        redirect_pc = true;
        redirect_target = csr_dpc;
        ifid_cur = {};
        idex_cur = {};
        debug_resume_sampled = false;
    }

    // CSRU counters use the old inhibit values and CSR writes later in this
    // same edge take priority, matching the nonblocking assignments in RTL.
    if (!csr_mcountinhibit_cycle || csr_debug_mode) {
        ++csr_mcycle;
    }
    if ((!csr_mcountinhibit_instr || csr_debug_mode) &&
        instrRetiringThisCycle) {
        ++csr_minstret;
    }

    //stall
    const HazardDecision hz = hazardUnit.resolve(ifid_cur, idex_cur);
    stall_pc   = hz.stall_pc;
    stall_ifid = hz.stall_ifid;
    bubble_idex = hz.bubble_idex;

    if (bubble_idex) {
        ++stall_count;
    }

    stageWB();
    stageMEM();

    stageEX();
    const bool executeStall = freezeFetchDecodeForExecuteStall();
    if (executeStall) {
        ++stall_count;
    } else if (!redirect_pc) {
        stageID();
    } else {
        // The redirected branch itself advances through EX.  Suppress the
        // younger sequential instruction that was sitting in IF/ID.
        idex_next = {};
    }

    // Spirit holds the IF/ID architectural state while IE/SC reports a
    // full-pipeline stall, but its IBU remains live: it can accept a pending
    // response and launch the next block prefetch.  Always clock the frontend
    // model and let stageIF() suppress instruction delivery while execute is
    // stalled.  Skipping stageIF() here delayed every fetch following a
    // VEU/HC stall and made the cycle trace diverge from RTL.
    stageIF();

}

void
PipelineCore::clockOneCycle()
{
    if (!cycleEvaluated) {
        return;
    }

    hcCbu.clock(hcIssue, hcResponse);

    // VCU registers csr_valid every edge from the request currently driven by
    // the CBU, so its control path can accept one request per cycle.  On the
    // first response of a two-shot VMADD/VMSUB, the CBU advances to its second
    // request at this edge.  Present that post-edge request to the endpoint so
    // the second response is visible on the immediately following edge,
    // while retaining hcRoutedRequests above as the pre-NBA trace value.
    brs::HcRoutedRequests endpointClockRequests = hcRoutedRequests;
    if (hcResponse.valid) {
        endpointClockRequests = hcRouter.routeRequest(
            hcCbu.evaluate({}).request);
    }
    if (sauEndpoint) {
        sauEndpoint->clockTick(
            endpointClockRequests.sau, sauMemoryResponse);
    }
    if (veuEndpoint) {
        veuEndpoint->clockTick(
            endpointClockRequests.veu, veuMemoryResponse);
    }
    sauMemoryResponse = {};
    veuMemoryResponse = {};
    commitNext();

    // Software/timer/external inputs are registered by CSRU/IRCU.  Updating
    // the samples after this cycle preserves that one-edge input latency.
    irq_external_sampled = irq_external_input;
    irq_software_sampled = irq_software_input;
    irq_timer_sampled = irq_timer_input;
    debug_halt_sampled = debug_halt_input;
    debug_resume_sampled = debug_resume_input;
    debug_halt_on_reset_latched =
        (debug_have_just_reset && debug_halt_on_reset_input) ||
        (!csr_debug_mode && debug_halt_on_reset_latched);
    debug_have_just_reset = false;
    debug_step_halt_requested = !csr_debug_mode &&
        ((csr_dcsr_step && instrRetiringThisCycle) ||
         debug_step_halt_requested);
    if (csr_debug_mode) {
        if (debug_instr_valid) {
            // PFU.sv toggles ready on every valid debug-instruction edge.
            debug_instr_ready = !debug_instr_ready;
        } else {
            debug_instr_ready = true;
        }
    } else {
        debug_instr_ready = true;
    }

    cycle++;
    regs[0] = 0;


    if (requestTimingFetch) {
        pc = frontend.getPC();
    }

    if (text_end_termination) {
        const bool reachedTextEnd = (requestTimingFetch || fetchInstr) ?
            (pc >= text_end) :
            ((((pc - text_start) >> 2) >= program.program_words));
        const bool veuQuiescent = !timingVeu.operationBusy();
        if (reachedTextEnd && pipelineEmpty() && veuQuiescent) {
            done_flag = true;
        }
    }
    cycleEvaluated = false;
}

void
PipelineCore::stepOneCycle()
{
    evaluateOneCycle();
    clockOneCycle();
}

void
PipelineCore::acceptDataResponse(uint32_t addr, uint32_t data, bool is_store)
{
    data_waiting = false;
    data_response_valid = true;
    data_response_addr = addr;
    data_response_value = data;
    data_response_is_store = is_store;
}

//差分
    void
    PipelineCore::traceRetire(uint32_t pc, uint32_t instr,
                              bool has_rd, uint8_t rd, uint32_t data)
    {
        if (!retire_trace_enable) {
            return;
        }

        std::ios old_state(nullptr);
        old_state.copyfmt(std::cout);

        std::cout << "RETIRE "
                  << "pc=0x" << std::hex << std::setw(8) << std::setfill('0') << pc
                  << " instr=0x" << std::hex << std::setw(8) << std::setfill('0') << instr;

        if (has_rd) {
            std::cout << " rd=x" << std::dec << static_cast<unsigned>(rd)
                      << " data=0x" << std::hex << std::setw(8) << std::setfill('0') << data;
        }

        std::cout << std::endl;
        std::cout.copyfmt(old_state);
    }

    void
    PipelineCore::traceStore(uint32_t pc, uint32_t instr,
                            uint32_t addr, uint32_t data)
    {
        if (!retire_trace_enable) {
            return;
        }

        std::ios old_state(nullptr);
        old_state.copyfmt(std::cout);

        std::cout << "STORE  "
                  << "pc=0x" << std::hex << std::setw(8) << std::setfill('0') << pc
                  << " instr=0x" << std::hex << std::setw(8) << std::setfill('0') << instr
                  << " addr=0x" << std::hex << std::setw(8) << std::setfill('0') << addr
                  << " data=0x" << std::hex << std::setw(8) << std::setfill('0') << data
                  << std::endl;

        std::cout.copyfmt(old_state);
    }


bool PipelineCore::done() const { return done_flag; }
uint64_t PipelineCore::getCycle() const { return cycle; }
uint32_t PipelineCore::getPC() const { return pc; }
uint32_t PipelineCore::getReg(int idx) const { return regs[idx]; }
uint32_t PipelineCore::getFpReg(int idx) const { return fp_regs[idx]; }
uint32_t PipelineCore::getCSR(uint16_t addr) const { return readCsr(addr); }
bool PipelineCore::ifidValid() const { return ifid_cur.valid; }
bool PipelineCore::idexValid() const { return idex_cur.valid; }
bool PipelineCore::exmemValid() const { return exmem_cur.valid; }
bool PipelineCore::memwbValid() const { return memwb_cur.valid; }

} // namespace gem5
