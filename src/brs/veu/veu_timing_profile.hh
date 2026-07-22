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
    std::string profileId = "fallback";
    bool matched = false;
};

class VeuTimingProfile
{
  public:
    void load(const std::string &path);
    bool loaded() const { return !rows.empty(); }
    VeuTimingSelection select(const std::string &op, uint32_t mode,
                              bool scalarEnabled,
                              const std::string &maskClass,
                              const std::string &sourceSet,
                              uint32_t fallbackLatency,
                              uint32_t fallbackII,
                              uint32_t fallbackFifoDepth,
                              uint32_t fallbackMaxOutstanding,
                              uint32_t fallbackVsuLatency) const;

  private:
    struct Row
    {
        std::string profileId;
        std::string op;
        std::string mode;
        std::string scalarEnabled;
        std::string maskClass;
        std::string sourceSet;
        uint32_t latency = 0;
        uint32_t initiationInterval = 0;
        std::string writePolicy;
        uint32_t fifoDepth = 0;
        uint32_t maxOutstandingReads = 0;
        uint32_t vsuLatency = 0;
    };

    std::vector<Row> rows;
};

} // namespace brs
} // namespace gem5

#endif
