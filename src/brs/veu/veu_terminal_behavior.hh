#ifndef __BRS_VEU_VEU_TERMINAL_BEHAVIOR_HH__
#define __BRS_VEU_VEU_TERMINAL_BEHAVIOR_HH__

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gem5
{
namespace brs
{

struct VeuTerminalSelection
{
    std::string behaviorId;
    std::string classification;
    std::string evidenceId;
    uint32_t statusClearCycles = 0;
    uint32_t lockFinishCycles = 0;
    uint32_t extraVfuAccepts = 0;
    uint32_t extraWrites = 0;
    uint32_t tailReads = 0;
    bool stuck = false;
};

class VeuTerminalBehavior
{
  public:
    void load(const std::string &path);
    bool loaded() const { return !rows.empty(); }
    std::optional<VeuTerminalSelection> select(
        const std::string &op, bool scalarEnabled,
        const std::string &maskClass, uint32_t chunkCount) const;

  private:
    struct Row
    {
        std::string behaviorId;
        std::string op;
        bool scalarEnabled = false;
        std::string maskClass;
        uint32_t chunkCount = 0;
        VeuTerminalSelection selection;
    };

    std::vector<Row> rows;
};

} // namespace brs
} // namespace gem5

#endif // __BRS_VEU_VEU_TERMINAL_BEHAVIOR_HH__
