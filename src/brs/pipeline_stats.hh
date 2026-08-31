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
    statistics::Scalar sau_issue_count;
    statistics::Scalar sau_retire_count;
    statistics::Scalar sau_csr_handshake_cycles;
    statistics::Scalar sau_memory_requests;
    statistics::Scalar sau_compute_wait_cycles;
    statistics::Scalar sau_writeback_wait_cycles;
    statistics::Scalar sau_operation_start_count;
    statistics::Scalar sau_operation_complete_count;
    statistics::Scalar sau_roi_start_cycle;
    statistics::Scalar sau_roi_end_cycle;
    statistics::Scalar sau_roi_start_retired_inst;
    statistics::Scalar sau_roi_end_retired_inst;
    statistics::Scalar sau_n_model_ticks;
    statistics::Scalar sau_n_operation_start_count;
    statistics::Scalar sau_n_operation_complete_count;
    statistics::Scalar sau_n_roi_start_cycle;
    statistics::Scalar sau_n_roi_end_cycle;
    statistics::Scalar sau_n_spad_read_requests_a;
    statistics::Scalar sau_n_spad_read_grants_a;
    statistics::Scalar sau_n_spad_read_responses_a;
    statistics::Scalar sau_n_spad_read_requests_b;
    statistics::Scalar sau_n_spad_read_grants_b;
    statistics::Scalar sau_n_spad_read_responses_b;
    statistics::Scalar sau_n_spad_read_requests_c;
    statistics::Scalar sau_n_spad_read_grants_c;
    statistics::Scalar sau_n_spad_read_responses_c;
    statistics::Scalar sau_n_spad_write_requests_d;
    statistics::Scalar sau_n_spad_write_grants_d;
    statistics::Scalar sau_n_b_buffer_hit_vectors;
    statistics::Scalar sau_n_b_buffer_switches;
    statistics::Scalar sau_n_d_pending_peak;
    statistics::Scalar sau_n_output_elements;
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
    statistics::Scalar veu_terminal_behavior_uses;
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
          ADD_STAT(sau_issue_count, "SAU instructions issued to the CBU"),
          ADD_STAT(sau_retire_count,
                   "SAU instructions completing their CBU CSR handshake sequence"),
          ADD_STAT(sau_csr_handshake_cycles,
                   "RV cycles stalled waiting for SAU CSR handshakes"),
          ADD_STAT(sau_memory_requests, "SAU SRAM requests issued"),
          ADD_STAT(sau_compute_wait_cycles,
                   "SAU cycles waiting while loading tensors"),
          ADD_STAT(sau_writeback_wait_cycles,
                   "SAU cycles waiting while writing output"),
          ADD_STAT(sau_operation_start_count, "SAU operations started"),
          ADD_STAT(sau_operation_complete_count,
                   "SAU operations completed"),
          ADD_STAT(sau_roi_start_cycle, "SAU ROI start endpoint cycle"),
          ADD_STAT(sau_roi_end_cycle, "SAU ROI end endpoint cycle"),
          ADD_STAT(sau_roi_start_retired_inst,
                   "Retired instructions observed at SAU ROI start"),
          ADD_STAT(sau_roi_end_retired_inst,
                   "Retired instructions observed at SAU ROI end"),
          ADD_STAT(sau_n_model_ticks,
                   "StreamingConvPipelineModel ticks"),
          ADD_STAT(sau_n_operation_start_count,
                   "sau_n operations started"),
          ADD_STAT(sau_n_operation_complete_count,
                   "sau_n operations completed"),
          ADD_STAT(sau_n_roi_start_cycle,
                   "sau_n ROI start endpoint cycle"),
          ADD_STAT(sau_n_roi_end_cycle,
                   "sau_n ROI end endpoint cycle"),
          ADD_STAT(sau_n_spad_read_requests_a,
                   "sau_n A scratchpad read requests"),
          ADD_STAT(sau_n_spad_read_grants_a,
                   "sau_n A scratchpad read grants"),
          ADD_STAT(sau_n_spad_read_responses_a,
                   "sau_n A scratchpad read responses"),
          ADD_STAT(sau_n_spad_read_requests_b,
                   "sau_n B scratchpad read requests"),
          ADD_STAT(sau_n_spad_read_grants_b,
                   "sau_n B scratchpad read grants"),
          ADD_STAT(sau_n_spad_read_responses_b,
                   "sau_n B scratchpad read responses"),
          ADD_STAT(sau_n_spad_read_requests_c,
                   "sau_n C scratchpad read requests"),
          ADD_STAT(sau_n_spad_read_grants_c,
                   "sau_n C scratchpad read grants"),
          ADD_STAT(sau_n_spad_read_responses_c,
                   "sau_n C scratchpad read responses"),
          ADD_STAT(sau_n_spad_write_requests_d,
                   "sau_n D scratchpad write requests"),
          ADD_STAT(sau_n_spad_write_grants_d,
                   "sau_n D scratchpad write grants"),
          ADD_STAT(sau_n_b_buffer_hit_vectors,
                   "sau_n B buffer hit vectors"),
          ADD_STAT(sau_n_b_buffer_switches,
                   "sau_n B buffer switches"),
          ADD_STAT(sau_n_d_pending_peak,
                   "sau_n peak D pending rows"),
          ADD_STAT(sau_n_output_elements,
                   "sau_n output elements written"),
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
          ADD_STAT(veu_chunks, "TimingVEU 128-bit chunks completed"),
          ADD_STAT(veu_memory_reads, "TimingVEU 128-bit memory reads issued"),
          ADD_STAT(veu_memory_writes, "TimingVEU 128-bit memory writes issued"),
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
          ADD_STAT(veu_store_priority_cycles, "TimingVEU store-priority arbitration cycles"),
          ADD_STAT(veu_reads_blocked_by_store, "TimingVEU reads blocked by stores"),
          ADD_STAT(veu_masked_writes, "TimingVEU partial-mask writes"),
          ADD_STAT(veu_zero_mask_skipped_writes, "TimingVEU zero-mask writes skipped"),
          ADD_STAT(veu_retries, "TimingVEU memory issue retries"),
          ADD_STAT(veu_unexpected_responses, "TimingVEU unexpected responses"),
          ADD_STAT(veu_profile_hits, "TimingVEU timing profile hits"),
          ADD_STAT(veu_profile_misses, "TimingVEU timing profile misses"),
          ADD_STAT(veu_profile_fallbacks, "TimingVEU timing profile fallbacks"),
          ADD_STAT(veu_terminal_behavior_uses,
                   "TimingVEU exact RTL terminal behaviors selected"),
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
