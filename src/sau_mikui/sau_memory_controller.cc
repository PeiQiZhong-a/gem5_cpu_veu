#include "sau_mikui/sau_memory_controller.hh"

namespace gem5::sau_mikui
{

SauMemoryController::SauMemoryController(bool strictTiming)
    : strict(strictTiming)
{
    reset();
}

void
SauMemoryController::reset()
{
    current = {};
    next = current;
    errorCounters = {};
}

SauMemoryControllerOutputs
SauMemoryController::evaluate() const
{
    return {
        current.request,
        current.readData,
        current.readValid,
        current.readLast,
        current.lastInstructionWriteDone,
        current.controller,
    };
}

void
SauMemoryController::computeNext(const SauMemoryControllerInputs &inputs)
{
    next = current;

    next.readData = current.responseData;
    if (inputs.response.valid) {
        next.responseData = inputs.response.readData;
    }
    next.readValid = current.readValidPipeline.back();
    next.readLast = current.readLastPipeline.back();
    for (unsigned stage = ControlDelay - 1; stage > 0; --stage) {
        next.readValidPipeline[stage] = current.readValidPipeline[stage - 1];
        next.readLastPipeline[stage] = current.readLastPipeline[stage - 1];
    }
    for (unsigned stage = ResponseDelay - 1; stage > 0; --stage) {
        next.responseExpectedPipeline[stage] =
            current.responseExpectedPipeline[stage - 1];
    }
    next.readValidPipeline[0] = inputs.readEnable;
    next.readLastPipeline[0] = inputs.readEnable && inputs.readLast;
    next.responseExpectedPipeline[0] = inputs.readEnable || inputs.writeEnable;

    if (strict) {
        const bool responseExpected = current.responseExpectedPipeline.back();
        if (inputs.response.valid && !responseExpected) {
            ++errorCounters.earlyResponses;
        } else if (!inputs.response.valid && responseExpected) {
            ++errorCounters.missingResponses;
        }
    }

    next.request = {};
    next.lastInstructionWriteDone = false;
    if (inputs.readEnable) {
        next.request.valid = true;
        next.request.address = inputs.address;
    } else if (inputs.writeEnable) {
        next.request.valid = true;
        next.request.address = inputs.address;
        next.request.writeStrobe = 0xffff;
        next.request.writeData = inputs.writeData;
        next.lastInstructionWriteDone =
            inputs.writeLast && inputs.lastInstruction &&
            inputs.schedulerState == SchedulerState::DOut;
    }

    if (inputs.readEnable || inputs.writeEnable) {
        switch (current.controller) {
            case MemoryControllerState::Idle:
                next.controller = MemoryControllerState::Requesting;
                break;
            case MemoryControllerState::Requesting:
                next.controller = MemoryControllerState::Waiting;
                break;
            case MemoryControllerState::Waiting:
                if ((inputs.readEnable && inputs.readLast) ||
                    (inputs.writeEnable && inputs.writeLast)) {
                    next.controller = MemoryControllerState::Idle;
                }
                break;
        }
    }

    if (next.request.isWrite() && next.request.writeStrobe != 0xffff) {
        ++errorCounters.illegalWriteMasks;
    }
}

void
SauMemoryController::commit()
{
    current = next;
}

} // namespace gem5::sau_mikui
