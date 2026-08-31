#ifndef __BRS_VEU_VEU_TIMING_PROFILE_HH__
#define __BRS_VEU_VEU_TIMING_PROFILE_HH__

#include <cstdint>
#include <string>
#include <vector>

namespace gem5
{
namespace brs
{

struct VeuTimingSelection
{
    uint32_t latency = 3;
    uint32_t initiationInterval = 1;
    uint32_t fifoDepth = 4;
    uint32_t maxOutstandingReads = 4;
    uint32_t vsuLatency = 1;
    uint32_t lockStartDelay = 1;
    uint32_t finishDrainCycles = 4;
    uint32_t operationCycles = 0;
    std::string profileId = "fallback";
    std::string timingSource = "default";
    std::string evidenceId = "builtin_veu_timing_config";
    std::string controlTimingSource = "default";
    std::string controlEvidenceId = "builtin_veu_timing_config";
    bool matched = false;
};

class VeuTimingProfile
{
  public:
    void load(const std::string &path);
    bool loaded() const { return !rows.empty(); }
    // RTL mode changes functional width/signedness, but not cycle timing.
    VeuTimingSelection select(const std::string &op, bool scalarEnabled,
                              const std::string &maskClass,
                              const std::string &sourceSet,
                              uint32_t chunkCount,
                              uint32_t fallbackLatency,
                              uint32_t fallbackII,
                              uint32_t fallbackFifoDepth,
                              uint32_t fallbackMaxOutstanding,
                              uint32_t fallbackVsuLatency,
                              uint32_t fallbackLockStartDelay,
                              uint32_t fallbackFinishDrainCycles) const;

  private:
    struct Row
    {
        std::string profileId;
        std::string op;
        // Retained as CSV evidence metadata; never used for timing selection.
        std::string mode;
        std::string scalarEnabled;
        std::string maskClass;
        std::string sourceSet;
        std::string chunkClass = "*";
        uint32_t latency = 0;
        uint32_t initiationInterval = 0;
        std::string writePolicy;
        uint32_t fifoDepth = 0;
        uint32_t maxOutstandingReads = 0;
        uint32_t vsuLatency = 0;
        uint32_t lockStartDelay = 0;
        uint32_t finishDrainCycles = 0;
        uint32_t operationCycles = 0;
        std::string timingSource = "legacy_default";
        std::string evidenceId;
        bool hasControlTiming = false;
    };

    std::vector<Row> rows;
};

} // namespace brs
} // namespace gem5

#endif
