#ifndef __SAU_N_IM2COL_TYPES_HH__
#define __SAU_N_IM2COL_TYPES_HH__

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace gem5::sau_n
{

inline constexpr uint64_t SchemaVersion = 1;
inline constexpr uint64_t BlockSize = 16;
inline constexpr uint64_t SpBanks = 16;
inline constexpr uint64_t ElemBits = 8;
inline constexpr uint64_t SpBankEntries = 4096;
inline constexpr uint64_t FifoDepth = 4;
inline constexpr uint64_t KernelPatternAll = 0xffff;

inline constexpr unsigned ReqValidHexDigits = 4;
inline constexpr unsigned ReqAddrHexDigits = 3;
inline constexpr unsigned RespValidHexDigits = 4;
inline constexpr unsigned RespDataHexDigits = 2;
inline constexpr unsigned FeedDataHexDigits = 32;
inline constexpr unsigned FeedMaskHexDigits = 4;

enum class Im2ColState : uint8_t
{
    Idle = 0,
    Issue = 1,
    Collect = 2,
    Push = 3,
    Next = 4,
    Done = 5,
};

std::string_view stateName(Im2ColState state);

struct ResolvedConfig
{
    uint64_t schemaVersion = SchemaVersion;
    std::string name;
    uint64_t n = 0;
    uint64_t c = 0;
    uint64_t h = 0;
    uint64_t w = 0;
    uint64_t outH = 0;
    uint64_t outW = 0;
    uint64_t kernelH = 0;
    uint64_t kernelW = 0;
    uint64_t strideH = 0;
    uint64_t strideW = 0;
    uint64_t dilationH = 0;
    uint64_t dilationW = 0;
    uint64_t padTop = 0;
    uint64_t padLeft = 0;
    uint64_t spadBase = 0;
    uint64_t cfgDwMode = 0;
    uint64_t cfgKernelPattern = KernelPatternAll;
    std::string inputGenerator = "tb_act_value_v1";
};

struct DerivedConfig
{
    uint64_t rowsPerWord = 0;
    uint64_t wWords = 0;
    uint64_t spatialWordsPerChannel = 0;
    uint64_t totalSpatialWords = 0;
    uint64_t hGroups = 0;
    uint64_t wGroups = 0;
    uint64_t expectedVectors = 0;
};

uint64_t checkedAdd(
    uint64_t lhs, uint64_t rhs, std::string_view description);
uint64_t checkedMultiply(
    uint64_t lhs, uint64_t rhs, std::string_view description);
uint64_t ceilDivide(uint64_t value, uint64_t divisor);
DerivedConfig validateAndDerive(const ResolvedConfig &config);

/**
 * The registered control subset needed to freeze cycle boundaries before the
 * complete tick model is implemented. Step 4 extends the register bundle but
 * must preserve this old-state/next-state/commit discipline.
 */
struct ControlRegisters
{
    Im2ColState state = Im2ColState::Idle;
    bool done = false;

    bool operator==(const ControlRegisters &other) const
    {
        return state == other.state && done == other.done;
    }
};

struct CycleObservation
{
    uint64_t cycle = 0;
    Im2ColState state = Im2ColState::Idle;
    bool busy = false;
    bool done = false;
};

bool busy(const ControlRegisters &registers);
ControlRegisters beginNext(const ControlRegisters &oldRegisters);
void commit(
    ControlRegisters &oldRegisters, const ControlRegisters &nextRegisters);
ControlRegisters cycleZeroRegisters();
ControlRegisters doneTransitionNext(const ControlRegisters &oldRegisters);
CycleObservation observe(
    uint64_t cycle, const ControlRegisters &registers);

inline constexpr std::array<std::string_view, 47> TraceFields = {
    "schema_version",
    "resolved_config_sha256",
    "cycle",
    "state",
    "busy",
    "done",
    "fifo_count",
    "fifo_rptr",
    "fifo_wptr",
    "req_valid",
    "req_addr_b00",
    "req_addr_b01",
    "req_addr_b02",
    "req_addr_b03",
    "req_addr_b04",
    "req_addr_b05",
    "req_addr_b06",
    "req_addr_b07",
    "req_addr_b08",
    "req_addr_b09",
    "req_addr_b10",
    "req_addr_b11",
    "req_addr_b12",
    "req_addr_b13",
    "req_addr_b14",
    "req_addr_b15",
    "resp_valid",
    "resp_data_b00",
    "resp_data_b01",
    "resp_data_b02",
    "resp_data_b03",
    "resp_data_b04",
    "resp_data_b05",
    "resp_data_b06",
    "resp_data_b07",
    "resp_data_b08",
    "resp_data_b09",
    "resp_data_b10",
    "resp_data_b11",
    "resp_data_b12",
    "resp_data_b13",
    "resp_data_b14",
    "resp_data_b15",
    "feed_valid",
    "feed_ready",
    "feed_data",
    "feed_mask",
};

} // namespace gem5::sau_n

#endif // __SAU_N_IM2COL_TYPES_HH__
