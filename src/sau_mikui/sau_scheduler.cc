#include "sau_mikui/sau_scheduler.hh"

#include "sau_mikui/sau_constants.hh"

namespace gem5::sau_mikui
{

void
SauScheduler::reset()
{
    current = {};
    next = current;
}

uint8_t
SauScheduler::targetFlowCount(const SauSchedulerInputs &inputs)
{
    if ((inputs.reuseMode & 0x3) != 0) {
        return inputs.flowTimes & 0x3f;
    }
    return static_cast<uint8_t>(((inputs.flowTimes & 0x3f) << 1) - 1) & 0x7f;
}

bool
SauScheduler::flowEnd(const SauSchedulerInputs &inputs) const
{
    if (current.core != SchedulerState::DOut) {
        return false;
    }
    if (inputs.lastInstruction) {
        return inputs.registerFileClear;
    }
    return inputs.updateFinished || inputs.writeFinished;
}

SchedulerState
SauScheduler::nextCoreState(const SauSchedulerInputs &inputs) const
{
    const bool reuse = (inputs.reuseMode & 0x3) != 0;
    const bool transpose = static_cast<uint8_t>(inputs.transposeMode) != 0;
    const bool execute =
        ((current.inputSwitch >> 0) & 1) ^ ((current.inputSwitch >> 1) & 1);
    const bool convMode = reuse && execute;

    switch (current.core) {
        case SchedulerState::Idle:
            if (current.instructionValid && inputs.convKernel >= 3) {
                return SchedulerState::RegisterLoad;
            }
            return current.instructionValid ? SchedulerState::FirstLoad
                                            : SchedulerState::Idle;
        case SchedulerState::RegisterLoad:
            if (transpose && inputs.loadDone) {
                return SchedulerState::TransposeLoad;
            }
            if (current.convReuse && inputs.loadDone) {
                return SchedulerState::ReuseLoad;
            }
            return SchedulerState::RegisterLoad;
        case SchedulerState::TransposeLoad:
            return (current.transposeLoadCount == SauConstants::Rows - 1 ||
                    inputs.loadDone)
                       ? SchedulerState::ReuseLoad
                       : SchedulerState::TransposeLoad;
        case SchedulerState::FirstLoad:
            if (inputs.executeFinished) {
                return SchedulerState::DOut;
            }
            return (convMode && inputs.loadDone) ? SchedulerState::ReuseLoad
                                                 : SchedulerState::FirstLoad;
        case SchedulerState::ReuseLoad:
            if (inputs.executeFinished) {
                return SchedulerState::DOut;
            }
            if (current.flowCount ==
                    static_cast<uint8_t>(targetFlowCount(inputs) - 1) &&
                inputs.loadDone && transpose) {
                return SchedulerState::FirstLoad;
            }
            return SchedulerState::ReuseLoad;
        case SchedulerState::DOut:
            return flowEnd(inputs) ? SchedulerState::Idle
                                   : SchedulerState::DOut;
    }
    return SchedulerState::Idle;
}

SauSchedulerOutputs
SauScheduler::evaluate(const SauSchedulerInputs &inputs) const
{
    return {
        current.core,
        static_cast<uint8_t>(current.inputSwitch & 0x3),
        static_cast<uint8_t>(current.flowCount & 0x7f),
        current.lastFlowTime,
        flowEnd(inputs),
        current.crossbarDone,
    };
}

void
SauScheduler::computeNext(const SauSchedulerInputs &inputs)
{
    next = current;
    const bool end = flowEnd(inputs);
    const bool dataLast = inputs.loadDone;
    const bool reuse = (inputs.reuseMode & 0x3) != 0;
    const bool transpose = static_cast<uint8_t>(inputs.transposeMode) != 0;
    const bool execute =
        ((current.inputSwitch >> 0) & 1) ^ ((current.inputSwitch >> 1) & 1);
    const bool convMode = reuse && execute;
    const uint8_t flows = targetFlowCount(inputs);
    const bool flowCounterClear = current.flowCount == flows && dataLast;

    next.instructionValid = inputs.start;
    next.dataLastD = dataLast;
    next.convReuse = inputs.convKernel >= 3;
    next.crossbarDone = end;
    next.core = nextCoreState(inputs);

    if (inputs.start) {
        next.flowCount = 0;
        next.lastFlowTime = false;
        next.transposeLoadCount = 0;
    }

    if (!inputs.start && current.core == SchedulerState::TransposeLoad) {
        next.transposeLoadCount =
            current.transposeLoadCount == SauConstants::Rows - 1
                ? 0
                : current.transposeLoadCount + 1;
    }

    if (!inputs.start && flowCounterClear) {
        next.flowCount = 0;
    } else if (!inputs.start && dataLast) {
        next.flowCount = static_cast<uint8_t>((current.flowCount + 1) & 0x7f);
    }

    if (inputs.start || inputs.lastFlowTimeClear) {
        next.lastFlowTime = false;
    } else if (flowCounterClear) {
        next.lastFlowTime = true;
    }

    switch (current.core) {
        case SchedulerState::Idle:
            next.inputSwitch =
                inputs.transposeMode == TransposeMode::Abtd ||
                        (inputs.transposeMode == TransposeMode::Abd &&
                         inputs.reuseMode == 2)
                    ? 3
                    : 0;
            break;
        case SchedulerState::FirstLoad:
        case SchedulerState::RegisterLoad:
            if (current.dataLastD && !convMode) {
                next.inputSwitch =
                    static_cast<uint8_t>(((current.inputSwitch & 0x2)) |
                                         ((~current.inputSwitch) & 0x1));
            }
            break;
        case SchedulerState::TransposeLoad:
            break;
        case SchedulerState::ReuseLoad:
            if (current.convReuse) {
                next.inputSwitch = 1;
            } else if (!(inputs.transposeMode == TransposeMode::Abd ||
                         static_cast<uint8_t>(inputs.transposeMode) ==
                             (inputs.reuseMode & 0x3))) {
                const uint8_t bit =
                    (~static_cast<uint8_t>(inputs.transposeMode)) & 1;
                next.inputSwitch = static_cast<uint8_t>(
                    (bit << 1) |
                    (bit ^ static_cast<uint8_t>(current.lastFlowTime)));
            }
            break;
        case SchedulerState::DOut:
            if (end) {
                next.inputSwitch = 0;
            }
            break;
    }

    (void)transpose;
}

void
SauScheduler::commit()
{
    current = next;
}

} // namespace gem5::sau_mikui
