#ifndef __SAU_MIKUI_SAU_MEMORY_CONTROLLER_HH__
#define __SAU_MIKUI_SAU_MEMORY_CONTROLLER_HH__

#include <array>
#include <cstdint>

#include "brs/memory/sram_128_protocol.hh"
#include "sau_mikui/sau_types.hh"

namespace gem5::sau_mikui
{

enum class MemoryControllerState : uint8_t
{
    Idle,
    Requesting,
    Waiting
};

struct SauMemoryControllerInputs
{
    uint32_t address = 0;
    bool readEnable = false;
    bool writeEnable = false;
    bool readLast = false;
    Beat128 writeData{};
    bool writeLast = false;
    bool lastInstruction = false;
    SchedulerState schedulerState = SchedulerState::Idle;
    brs::Sram128Response response{};
};

struct SauMemoryControllerOutputs
{
    brs::Sram128Request request{};
    Beat128 readData{};
    bool readValid = false;
    bool readLast = false;
    bool lastInstructionWriteDone = false;
    MemoryControllerState state = MemoryControllerState::Idle;
};

struct SauMemoryControllerErrors
{
    uint64_t earlyResponses = 0;
    uint64_t missingResponses = 0;
    uint64_t illegalWriteMasks = 0;

    bool
    any() const
    {
        return earlyResponses || missingResponses || illegalWriteMasks;
    }
};

class SauMemoryController
{
  public:
    explicit SauMemoryController(bool strictTiming = true);

    void reset();
    SauMemoryControllerOutputs evaluate() const;
    void computeNext(const SauMemoryControllerInputs &inputs);
    void commit();

    const SauMemoryControllerErrors &
    errors() const
    {
        return errorCounters;
    }
    bool
    timingError() const
    {
        return errorCounters.any();
    }

  private:
    static constexpr unsigned ResponseDelay = SauConstants::SramDelay + 1;
    static constexpr unsigned ControlDelay = ResponseDelay + 1;

    struct State
    {
        MemoryControllerState controller = MemoryControllerState::Idle;
        brs::Sram128Request request{};
        Beat128 responseData{};
        Beat128 readData{};
        bool readValid = false;
        bool readLast = false;
        bool lastInstructionWriteDone = false;
        std::array<bool, ControlDelay> readValidPipeline{};
        std::array<bool, ControlDelay> readLastPipeline{};
        std::array<bool, ResponseDelay> responseExpectedPipeline{};
    } current, next;

    bool strict;
    SauMemoryControllerErrors errorCounters{};
};

} // namespace gem5::sau_mikui

#endif // __SAU_MIKUI_SAU_MEMORY_CONTROLLER_HH__
