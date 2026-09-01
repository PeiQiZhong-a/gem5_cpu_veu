#ifndef __SAU_MIKUI_SAU_SCHEDULER_HH__
#define __SAU_MIKUI_SAU_SCHEDULER_HH__

#include <cstdint>

#include "sau_mikui/sau_types.hh"

namespace gem5::sau_mikui
{

struct SauSchedulerInputs
{
    bool start = false;
    uint8_t convKernel = 0;
    uint8_t reuseMode = 0;
    TransposeMode transposeMode = TransposeMode::Abd;
    uint8_t flowTimes = 0;
    bool shift = false;
    bool lastInstruction = false;
    bool loadDone = false;
    bool executeFinished = false;
    bool updateFinished = false;
    bool writeFinished = false;
    bool registerFileClear = false;
    bool lastInstructionWriteDone = false;
    bool lastFlowTimeClear = false;
};

struct SauSchedulerOutputs
{
    SchedulerState state = SchedulerState::Idle;
    uint8_t inputSwitch = 0;
    uint8_t flowCount = 0;
    bool lastFlowTime = false;
    bool flowEnd = false;
    bool crossbarDone = false;
};

class SauScheduler
{
  public:
    void reset();
    SauSchedulerOutputs evaluate(const SauSchedulerInputs &inputs) const;
    void computeNext(const SauSchedulerInputs &inputs);
    void commit();

  private:
    struct State
    {
        SchedulerState core = SchedulerState::Idle;
        bool instructionValid = false;
        bool dataLastD = false;
        bool convReuse = false;
        uint8_t transposeLoadCount = 0;
        uint8_t flowCount = 0;
        uint8_t inputSwitch = 0;
        bool lastFlowTime = false;
        bool crossbarDone = false;
    } current, next;

    static uint8_t targetFlowCount(const SauSchedulerInputs &inputs);
    bool flowEnd(const SauSchedulerInputs &inputs) const;
    SchedulerState nextCoreState(const SauSchedulerInputs &inputs) const;
};

} // namespace gem5::sau_mikui

#endif // __SAU_MIKUI_SAU_SCHEDULER_HH__
