#include "sau_mikui/sau_feeder.hh"

#include <algorithm>

namespace gem5::sau_mikui
{

void
SauFeeder::reset()
{
    current = {};
    next = current;
}

SauFeederOutputs
SauFeeder::evaluate() const
{
    auto output = current.output;
    output.inputState = current.inputState;
    return output;
}

Row8
SauFeeder::toRow8(const Beat128 &beat)
{
    Row8 result{};
    for (unsigned i = 0; i < result.size(); ++i) {
        result[i] = static_cast<int8_t>(beat[i]);
    }
    return result;
}

Row16
SauFeeder::toBias(const Beat128 &beat, const Beat128 &previous, bool depthwise)
{
    Row16 result{};
    if (depthwise) {
        const int16_t value =
            static_cast<int16_t>(static_cast<uint16_t>(beat[6]) |
                                 (static_cast<uint16_t>(beat[7]) << 8));
        result.fill(value);
        return result;
    }
    std::array<uint8_t, SauConstants::SramBeatBytes * 2> combined{};
    std::copy(previous.begin(), previous.end(), combined.begin());
    std::copy(beat.begin(), beat.end(),
              combined.begin() + SauConstants::SramBeatBytes);
    for (unsigned i = 0; i < result.size(); ++i) {
        result[i] = static_cast<int16_t>(
            static_cast<uint16_t>(combined[i * 2]) |
            (static_cast<uint16_t>(combined[i * 2 + 1]) << 8));
    }
    return result;
}

Beat128
SauFeeder::outputBeat(const State &state, uint8_t count)
{
    Beat128 beat{};
    const bool high = state.command.shift && (count & 1);
    const unsigned row = state.command.shift ? count / 2 : count;
    if (row >= SauConstants::Rows) {
        return beat;
    }
    const Row16 &data = state.outputBuffer[row];
    if (state.command.shift) {
        const unsigned first = high ? SauConstants::Cols / 2 : 0;
        for (unsigned i = 0; i < SauConstants::Cols / 2; ++i) {
            const uint16_t value = static_cast<uint16_t>(data[first + i]);
            beat[i * 2] = value & 0xff;
            beat[i * 2 + 1] = value >> 8;
        }
    } else {
        for (unsigned i = 0; i < SauConstants::Cols; ++i) {
            beat[i] = static_cast<uint8_t>(data[i] & 0xff);
        }
    }
    return beat;
}

void
SauFeeder::computeNext(const SauFeederInputs &inputs)
{
    next = current;
    next.output = {};
    next.output.inputState = current.inputState;

    if (inputs.start) {
        next.command = inputs.command;
        next.inputState = FeederInputState::NoInput;
        next.labels = {};
        next.registerLoaded = false;
        next.registerReadWindows = 0;
    }

    if (!inputs.start) {
        for (unsigned stage = LabelDelay; stage > 0; --stage) {
            next.labels[stage] = current.labels[stage - 1];
        }
        next.labels[0] = {
            inputs.schedulerState,
            static_cast<uint8_t>(inputs.inputSwitch & 0x3),
            inputs.lastFlowTime,
        };
    }
    const Label &label = current.labels.back();
    next.output.inputSwitch = label.inputSwitch;

    const bool convolution = current.command.convKernel >= 3;
    const bool depthwise = current.command.registerMode == 2;
    const bool transposed =
        current.command.transposeMode != TransposeMode::Abd;
    if (inputs.memoryValid) {
        if (label.state == SchedulerState::RegisterLoad) {
            next.output.registerWriteValid = true;
            next.output.registerWriteData = inputs.memoryData;
            next.registerLoaded = true;
        }

        if (label.lastFlowTime) {
            next.output.cValid = true;
            next.output.c =
                toBias(inputs.memoryData, current.memoryDataD, depthwise);
        } else if (convolution) {
            next.output.bValid = true;
            next.output.b = toRow8(inputs.memoryData);
            next.output.bLast = inputs.memoryLast;
        } else {
            const Row8 data = toRow8(inputs.memoryData);
            if ((label.inputSwitch & 1) == 0) {
                next.output.aValid = true;
                next.output.a = data;
                next.output.aLast = inputs.memoryLast;
            } else {
                next.output.bValid = true;
                next.output.b = data;
                next.output.bLast = inputs.memoryLast;
            }
        }
    }

    // Consecutive SRAM bursts need not contain an idle cycle.  The delayed
    // read-last marker therefore starts the next transaction just as an
    // actual low-to-high valid transition does.
    const bool memoryStart =
        inputs.memoryValid && (!current.memoryValidD || current.memoryLastD);
    if (current.inputState == FeederInputState::NoInput) {
        if (!transposed && inputs.memoryLast) {
            next.inputState = FeederInputState::TwoInput;
        } else if (memoryStart && !convolution) {
            next.inputState = FeederInputState::OneInput;
        } else if (transposed && convolution && inputs.memoryLast) {
            next.inputState = FeederInputState::OneInput;
        }
    } else if (current.inputState == FeederInputState::OneInput) {
        if (current.lastFlowD && !label.lastFlowTime) {
            next.inputState = FeederInputState::NoInput;
        } else if (convolution && label.state == SchedulerState::ReuseLoad) {
            next.inputState = FeederInputState::TwoInput;
        }
    } else if (current.lastFlowD && !label.lastFlowTime && convolution) {
        next.inputState = FeederInputState::NoInput;
    } else if (!transposed && current.command.reuseMode == 0 &&
               inputs.memoryLast) {
        next.inputState = FeederInputState::OneInput;
    }

    const uint8_t registerWindows =
        depthwise ? std::max<uint8_t>(1, current.command.flowLoopTimes) : 1;
    if (convolution && current.registerLoaded &&
        label.state == SchedulerState::ReuseLoad && memoryStart &&
        current.registerReadWindows < registerWindows) {
        next.output.registerReadEnable = true;
        next.registerReadWindows = current.registerReadWindows + 1;
    }
    if (convolution && inputs.registerValid) {
        next.convWindowData = inputs.registerData;
        next.convWindowActive = true;
    }
    if (convolution && (current.convWindowActive || inputs.registerValid)) {
        const Row8 data = inputs.registerValid ? inputs.registerData
                                               : current.convWindowData;
        const uint8_t cycles =
            current.command.convKernel == 3
                ? 9
                : (current.command.convKernel == 5 ? 25 : 49);
        next.output.aValid = true;
        next.output.a = data;
        next.output.aLast = current.convWindowCount == cycles - 1;
        next.shiftCount = static_cast<uint8_t>(current.shiftCount + 1);
        if (next.output.aLast) {
            next.convWindowActive = false;
            next.convWindowCount = 0;
        } else {
            next.convWindowCount = current.convWindowCount + 1;
        }
    }

    if (inputs.resultValid) {
        const uint8_t row = inputs.resultIndexValid ? inputs.resultRowIndex
                                                    : current.outputWriteRow;
        next.outputBuffer[row % SauConstants::Rows] = inputs.resultData;
        next.outputWriteRow = (row + 1) % SauConstants::Rows;
        next.outputBufferValid = true;
    }

    const bool keepMode = current.command.flowMode == FlowMode::Retain ||
                          current.command.flowMode == FlowMode::Tretain;
    const bool currentInstructionOut =
        current.command.lastInstruction && !keepMode;
    const bool writeTrigger = inputs.resultValid && inputs.resultLast &&
                              inputs.schedulerState == SchedulerState::DOut &&
                              currentInstructionOut;
    // The RTL update FSM retires a non-output/retained instruction from the
    // execute-done path.  This is distinct from the last SRAM write pulse.
    next.output.updateFinished = inputs.resultLast && !currentInstructionOut;
    if (inputs.executeDone && keepMode) {
        next.output.updateFinished = true;
    }
    if (writeTrigger && !current.writeAddressActive) {
        next.writeAddressActive = true;
        next.writeAddressCount = 0;
        next.writeStartPipeline[0] = true;
    }
    for (unsigned stage = SauConstants::AddressDelay - 1; stage > 0; --stage) {
        next.writeStartPipeline[stage] = current.writeStartPipeline[stage - 1];
    }
    next.writeStartPipeline[0] = writeTrigger && !current.writeAddressActive;

    const uint8_t beats =
        current.command.shift ? SauConstants::Rows * 2 : SauConstants::Rows;
    if (current.writeAddressActive) {
        next.output.writeAddressValid = true;
        next.output.writeAddressLast = current.writeAddressCount == beats - 1;
        if (next.output.writeAddressLast) {
            next.writeAddressActive = false;
            next.writeAddressCount = 0;
        } else {
            next.writeAddressCount = current.writeAddressCount + 1;
        }
    }
    if (current.writeStartPipeline.back()) {
        next.writeDataActive = true;
        next.writeDataCount = 0;
    }
    if (current.writeDataActive) {
        next.output.writeDataValid = true;
        next.output.writeData = outputBeat(current, current.writeDataCount);
        next.output.writeDataLast = current.writeDataCount == beats - 1;
        if (next.output.writeDataLast) {
            next.writeDataActive = false;
            next.writeDataCount = 0;
            next.outputBufferValid = false;
        } else {
            next.writeDataCount = current.writeDataCount + 1;
        }
    }

    next.output.lastFlowTimeClear =
        inputs.resultLast || (current.lastFlowD && !label.lastFlowTime);
    next.memoryValidD = inputs.memoryValid;
    next.memoryLastD = inputs.memoryLast;
    next.memoryDataD = inputs.memoryData;
    next.lastFlowD = label.lastFlowTime;
}

void
SauFeeder::commit()
{
    current = next;
}

} // namespace gem5::sau_mikui
