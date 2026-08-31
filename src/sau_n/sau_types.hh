#ifndef __SAU_N_SAU_TYPES_HH__
#define __SAU_N_SAU_TYPES_HH__

#include <cstdint>
#include <string>
#include <string_view>

#include "sau_n/im2col_types.hh"

namespace gem5::sau_n
{

inline constexpr uint64_t SauRows = 16;
inline constexpr uint64_t SauColumns = 16;
inline constexpr uint64_t SauKernelHeight = 3;
inline constexpr uint64_t SauKernelWidth = 3;
inline constexpr uint64_t SauMaxChannels = 63;
inline constexpr uint64_t SauMaxOutChannels = 16;
inline constexpr uint64_t SauMaxCutbit = 23;
inline constexpr bool SauCycleAnchorsProvisional = false;

enum class PipelineState : uint8_t
{
    Idle = 0,
    CollectTile = 1,
    LaunchSa = 2,
    StreamK = 3,
    WaitResult = 4,
    DrainOutput = 5,
    Done = 6,
};

enum class BankArbitrationPolicy : uint8_t
{
    ADB = 0,
};

struct SharedSpadConfig
{
    bool configured = false;
    uint64_t aBase = 0;
    uint64_t aRows = 0;
    uint64_t bBase = 0;
    uint64_t bRows = 0;
    uint64_t cBase = 0;
    uint64_t cRows = 0;
    uint64_t dBase = 0;
    uint64_t dRows = 0;
    uint64_t bBufferDepth = 0;
    uint64_t dPendingRows = 1;
    bool weightReuse = true;
    BankArbitrationPolicy arbitration = BankArbitrationPolicy::ADB;
};

enum class SauInputProtocol : uint8_t
{
    StrictRtlContinuous = 0,
    ElasticBubbleEnabled = 1,
};

std::string_view pipelineStateName(PipelineState state);

struct PipelineResolvedConfig
{
    uint64_t schemaVersion = SchemaVersion;
    std::string name;
    ResolvedConfig im2col;
    uint64_t outChannels = 0;
    uint64_t cutbit = 0;
    std::string weightGenerator = "tb_weight_value_v1";
    std::string biasGenerator = "tb_bias_value_v1";
    SharedSpadConfig sharedSpad;
};

struct PipelineDerivedConfig
{
    DerivedConfig im2col;
    uint64_t k = 0;
    uint64_t expectedTiles = 0;
    uint64_t expectedOutputs = 0;
    uint64_t expectedMacs = 0;
    SharedSpadConfig sharedSpad;
};

PipelineDerivedConfig validateAndDerive(
    const PipelineResolvedConfig &config);

struct OutputReadyConfig
{
    uint64_t period = 1;
    uint64_t highCycles = 1;
};

void validateOutputReady(const OutputReadyConfig &config);
bool outputReady(uint64_t cycle, const OutputReadyConfig &config = {});
uint64_t peIndex(uint64_t row, uint64_t column);

struct PipelineDrainStatus
{
    bool im2colCanFeed = false;
    bool im2colFifoEmpty = false;
    bool tileBufferEmpty = false;
    bool sauIdle = false;
    uint64_t completedTiles = 0;
    uint64_t writtenOutputs = 0;
    bool outputPending = false;
};

bool pipelineDrained(
    const PipelineDrainStatus &status,
    const PipelineDerivedConfig &derived);

} // namespace gem5::sau_n

#endif // __SAU_N_SAU_TYPES_HH__
