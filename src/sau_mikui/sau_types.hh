#ifndef __SAU_MIKUI_SAU_TYPES_HH__
#define __SAU_MIKUI_SAU_TYPES_HH__

#include <array>
#include <cstdint>
#include <string_view>

#include "sau_mikui/sau_constants.hh"

namespace gem5::sau_mikui
{

using Beat128 = std::array<uint8_t, SauConstants::SramBeatBytes>;
using Row8 = std::array<int8_t, SauConstants::Cols>;
using Row16 = std::array<int16_t, SauConstants::Cols>;
using Row24 = std::array<int32_t, SauConstants::Cols>;

enum class CalculateMode : uint8_t
{
    Matmul = 0,
    Conv = 1,
    Transposer = 2,
    Add = 3
};

enum class TransposeMode : uint8_t
{
    Abd = 0,
    Atbd = 1,
    Abtd = 2,
    Abdt = 3
};

enum class FlowMode : uint8_t
{
    Cnormal = 0,
    Ctrans = 1,
    Retain = 2,
    Tretain = 3
};

enum class SchedulerState : uint8_t
{
    Idle = 0,
    FirstLoad = 1,
    RegisterLoad = 2,
    TransposeLoad = 3,
    ReuseLoad = 4,
    DOut = 5
};

struct PeInstruction
{
    uint8_t shiftMode = 0;
    bool keepMode = false;
    CalculateMode operation = CalculateMode::Matmul;
};

struct SauCommand
{
    uint8_t registerMode = 0;
    uint8_t convKernel = 0;
    uint8_t reuseMode = 0;
    bool stride = false;
    bool shift = false;
    TransposeMode transposeMode = TransposeMode::Abd;
    uint8_t cutbit = 0;
    bool lastInstruction = false;

    uint8_t verticalXStep = 0;
    uint8_t horizontalXStep = 0;
    uint8_t outputXStep = 0;
    uint8_t verticalChannelStep = 0;
    uint8_t horizontalChannelStep = 0;
    uint8_t outputChannelStep = 0;

    uint32_t verticalBase = 0;
    uint32_t horizontalBase = 0;
    uint32_t outputBase = 0;
    uint32_t biasBase = 0;
    uint8_t instructionId = 0;
    CalculateMode operation = CalculateMode::Matmul;
    FlowMode flowMode = FlowMode::Cnormal;
    uint8_t flowLoopTimes = 0;
};

constexpr std::string_view
toString(SchedulerState state)
{
    switch (state) {
        case SchedulerState::Idle:
            return "IDLE";
        case SchedulerState::FirstLoad:
            return "FIRST_LOAD";
        case SchedulerState::RegisterLoad:
            return "REGISTER_LOAD";
        case SchedulerState::TransposeLoad:
            return "TRANSPOSE_LOAD";
        case SchedulerState::ReuseLoad:
            return "REUSE_LOAD";
        case SchedulerState::DOut:
            return "D_OUT";
    }
    return "UNKNOWN";
}

} // namespace gem5::sau_mikui

#endif // __SAU_MIKUI_SAU_TYPES_HH__
