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
    statistics::Scalar veu_status_active_cycles;
    statistics::Scalar veu_lock_active_cycles;
    statistics::Scalar veu_current_outstanding_reads;
    statistics::Scalar veu_max_outstanding_reads;
    statistics::Scalar veu_fifo1_max_occupancy;
    statistics::Scalar veu_fifo2_max_occupancy;
    statistics::Scalar veu_fifo3_max_occupancy;
    statistics::Scalar veu_fifo_empty_stalls;
    statistics::Scalar veu_fifo_full_stalls;
    statistics::Scalar veu_vfu_accepted;
    statistics::Scalar veu_vfu_completed;
    statistics::Scalar veu_vfu_max_in_flight;
    statistics::Scalar veu_vfu_ii_stalls;
    statistics::Scalar veu_vsu_queue_stalls;
    statistics::Scalar veu_store_priority_cycles;
    statistics::Scalar veu_reads_blocked_by_store;
    statistics::Scalar veu_masked_writes;
    statistics::Scalar veu_zero_mask_skipped_writes;
    statistics::Scalar veu_retries;
    statistics::Scalar veu_unexpected_responses;
    statistics::Scalar veu_profile_hits;
    statistics::Scalar veu_profile_misses;
    statistics::Scalar veu_profile_fallbacks;
    statistics::Scalar veu_timing_rtl_sim_uses;
    statistics::Scalar veu_timing_legacy_uses;
    statistics::Scalar veu_timing_default_uses;
    statistics::Scalar veu_control_timing_rtl_sim_uses;
    statistics::Scalar veu_control_timing_default_uses;
    statistics::Scalar veu_zero_length_noops;
    statistics::Scalar veu_illegal_operations;

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
          ADD_STAT(veu_memory_writes, "TimingVEU 256-bit memory writes issued"),
          ADD_STAT(veu_status_active_cycles, "TimingVEU status busy cycles"),
          ADD_STAT(veu_lock_active_cycles, "TimingVEU shared SRAM lock cycles"),
          ADD_STAT(veu_current_outstanding_reads, "Current TimingVEU outstanding reads"),
          ADD_STAT(veu_max_outstanding_reads, "Maximum TimingVEU outstanding reads"),
          ADD_STAT(veu_fifo1_max_occupancy, "Maximum TimingVEU source-1 FIFO occupancy"),
          ADD_STAT(veu_fifo2_max_occupancy, "Maximum TimingVEU source-2 FIFO occupancy"),
          ADD_STAT(veu_fifo3_max_occupancy, "Maximum TimingVEU source-3 FIFO occupancy"),
          ADD_STAT(veu_fifo_empty_stalls, "TimingVEU VFU stalls waiting for source data"),
          ADD_STAT(veu_fifo_full_stalls, "TimingVEU load stalls due to full source FIFO"),
          ADD_STAT(veu_vfu_accepted, "TimingVEU VFU tokens accepted"),
          ADD_STAT(veu_vfu_completed, "TimingVEU VFU tokens completed"),
          ADD_STAT(veu_vfu_max_in_flight, "Maximum TimingVEU VFU tokens in flight"),
          ADD_STAT(veu_vfu_ii_stalls, "TimingVEU VFU initiation interval stalls"),
          ADD_STAT(veu_vsu_queue_stalls, "TimingVEU VSU queue stalls"),
          ADD_STAT(veu_store_priority_cycles,
                   "TimingVEU cycles where a store owns the VSPBU issue slot"),
          ADD_STAT(veu_reads_blocked_by_store,
                   "TimingVEU store-priority cycles with an otherwise issuable read"),
          ADD_STAT(veu_masked_writes, "TimingVEU partial-mask writes"),
          ADD_STAT(veu_zero_mask_skipped_writes, "TimingVEU zero-mask writes skipped"),
          ADD_STAT(veu_retries, "TimingVEU memory issue retries"),
          ADD_STAT(veu_unexpected_responses, "TimingVEU unexpected responses"),
          ADD_STAT(veu_profile_hits, "TimingVEU timing profile hits"),
          ADD_STAT(veu_profile_misses, "TimingVEU timing profile misses"),
          ADD_STAT(veu_profile_fallbacks, "TimingVEU timing profile fallbacks"),
          ADD_STAT(veu_timing_rtl_sim_uses,
                   "TimingVEU operations using RTL simulation timing"),
          ADD_STAT(veu_timing_legacy_uses,
                   "TimingVEU operations using legacy profile timing"),
          ADD_STAT(veu_timing_default_uses,
                   "TimingVEU operations using default timing"),
          ADD_STAT(veu_control_timing_rtl_sim_uses,
                   "TimingVEU operations using RTL simulation control timing"),
          ADD_STAT(veu_control_timing_default_uses,
                   "TimingVEU operations using default control timing"),
          ADD_STAT(veu_zero_length_noops, "TimingVEU VLEN=0 no-ops"),
          ADD_STAT(veu_illegal_operations, "TimingVEU illegal operations")
    {
    }
};

} // namespace gem5

#endif
