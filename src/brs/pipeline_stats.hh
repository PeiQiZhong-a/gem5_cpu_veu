#ifndef __BRS_PIPELINE_STATS_HH__
#define __BRS_PIPELINE_STATS_HH__

#include "base/statistics.hh"

namespace gem5
{

struct PipelineStats : public statistics::Group
{
    statistics::Scalar cycle_count;
    statistics::Scalar retired_inst_count;
    statistics::Scalar stall_count;
    statistics::Scalar forward_count;
    statistics::Scalar flush_count;
    statistics::Scalar icache_hit_count;
    statistics::Scalar icache_miss_count;
    statistics::Scalar ibus_req_count;
    statistics::Scalar fetch_fifo_flush_count;
    statistics::Scalar aligned_instr_count;
    statistics::Scalar veu_issue_count;
    statistics::Scalar veu_complete_count;
    statistics::Scalar veu_csr_handshake_cycles;
    statistics::Scalar rv_dmem_blocked_by_veu_cycles;
    statistics::Scalar veu_operation_start_count;
    statistics::Scalar veu_operation_complete_count;
    statistics::Scalar veu_busy_cycles;
    statistics::Scalar veu_load_wait_cycles;
    statistics::Scalar veu_execute_cycles;
    statistics::Scalar veu_store_wait_cycles;
    statistics::Scalar veu_chunks;
    statistics::Scalar veu_memory_reads;
    statistics::Scalar veu_memory_writes;

    PipelineStats(statistics::Group *parent)
        : statistics::Group(parent),
          ADD_STAT(cycle_count, "Cycles executed by pipeline"),
          ADD_STAT(retired_inst_count, "Instructions retired by pipeline"),
          ADD_STAT(stall_count, "Pipeline stall count"),
          ADD_STAT(forward_count, "Forwarding count"),
          ADD_STAT(flush_count, "Pipeline flush count"),
          ADD_STAT(icache_hit_count, "Simple I-cache hit count"),
          ADD_STAT(icache_miss_count, "Simple I-cache miss count"),
          ADD_STAT(ibus_req_count, "Frontend instruction burst requests"),
          ADD_STAT(fetch_fifo_flush_count, "Frontend instruction FIFO flushes"),
          ADD_STAT(aligned_instr_count, "Instructions emitted by frontend aligner"),
          ADD_STAT(veu_issue_count, "VEU instructions issued to the CBU"),
          ADD_STAT(veu_complete_count,
                   "VEU instructions completing their CBU CSR handshake sequence"),
          ADD_STAT(veu_csr_handshake_cycles,
                   "RV cycles stalled waiting for VEU CSR handshakes"),
          ADD_STAT(rv_dmem_blocked_by_veu_cycles,
                   "RV DMem cycles blocked while TimingVEU owns shared DMem"),
          ADD_STAT(veu_operation_start_count,
                   "TimingVEU vector operations started"),
          ADD_STAT(veu_operation_complete_count,
                   "TimingVEU vector operations completed"),
          ADD_STAT(veu_busy_cycles,
                   "TimingVEU cycles with a vector operation in progress"),
          ADD_STAT(veu_load_wait_cycles, "TimingVEU cycles waiting for load responses"),
          ADD_STAT(veu_execute_cycles, "TimingVEU execute cycles"),
          ADD_STAT(veu_store_wait_cycles, "TimingVEU cycles waiting for store responses"),
          ADD_STAT(veu_chunks, "TimingVEU 256-bit chunks completed"),
          ADD_STAT(veu_memory_reads, "TimingVEU 256-bit memory reads issued"),
          ADD_STAT(veu_memory_writes, "TimingVEU 256-bit memory writes issued")
    {
    }
};

} // namespace gem5

#endif
