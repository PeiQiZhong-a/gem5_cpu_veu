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
          ADD_STAT(aligned_instr_count, "Instructions emitted by frontend aligner")
    {
    }
};

} // namespace gem5

#endif
