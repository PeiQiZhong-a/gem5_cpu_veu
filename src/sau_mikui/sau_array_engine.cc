#include "sau_mikui/sau_array_engine.hh"

#include <algorithm>

namespace gem5::sau_mikui
{

uint16_t
SauArrayEngine::calculateCycles(uint8_t kernel)
{
    switch (kernel & 0x7) {
        case 3:
            return 9;
        case 5:
            return 25;
        case 7:
            return 49;
        default:
            return SauConstants::Rows;
    }
}

void
SauArrayEngine::reset()
{
    current = {};
    next = current;
    array.reset();
}

SauArrayEngineOutputs
SauArrayEngine::evaluate() const
{
    auto output = current.output;
    output.state = current.controller;
    return output;
}

void
SauArrayEngine::computeNext(const SauArrayEngineInputs &inputs)
{
    next = current;
    next.output = {};
    next.output.state = current.controller;
    const auto arrayOut = array.evaluate();

    if (inputs.start) {
        if (current.controller == ArrayEngineState::Work) {
            // csr.sv keeps the newly written command visible. SA_ENGINE
            // ignores ins_valid_i in WORK and captures it at OS_valid[0].
            next.pendingCommand = inputs.command;
            next.pendingCommandValid = true;
        } else {
            next.command = inputs.command;
            next.inputCycles = 0;
            next.completedWindows = 0;
            next.drainCycles = 0;
            next.outputRow = 0;
            next.depthwiseMask = 1;
            next.sawLast = false;
            next.directResults = {};
            if (current.controller == ArrayEngineState::Idle ||
                current.controller == ArrayEngineState::Done) {
                next.suppressOutput = false;
            }
        }
    }
    if (inputs.aValid) {
        next.activation = inputs.a;
    }
    if (inputs.bValid) {
        next.weight = inputs.b;
    }
    if (inputs.cValid) {
        next.bias = inputs.c;
    }

    const bool convolution = current.command.convKernel >= 3;
    const bool inputValid =
        convolution ? inputs.aValid : (inputs.aValid || inputs.bValid);
    switch (current.controller) {
        case ArrayEngineState::Idle:
            if (inputValid) {
                next.controller = ArrayEngineState::Start;
            }
            break;
        case ArrayEngineState::Start:
            if (inputValid &&
                current.inputCycles + 1 >=
                    calculateCycles(current.command.convKernel)) {
                next.controller = ArrayEngineState::Work;
            } else if (!inputValid && current.inputCycles == 0) {
                next.controller = ArrayEngineState::Idle;
            }
            break;
        case ArrayEngineState::Work:
            if (arrayOut.rowValid[0] && !inputValid) {
                next.controller = ArrayEngineState::Storage;
                next.suppressOutput =
                    current.command.flowMode == FlowMode::Retain ||
                    current.command.flowMode == FlowMode::Tretain;
                next.output.storageReady = !next.suppressOutput;
            }
            break;
        case ArrayEngineState::Storage:
            if (current.suppressOutput && inputValid) {
                next.controller = ArrayEngineState::Start;
                next.suppressOutput = false;
            } else if (current.outputRow == SauConstants::Rows) {
                next.controller = ArrayEngineState::Done;
            }
            break;
        case ArrayEngineState::Done:
            next.output.calculateFinish = true;
            next.controller =
                inputValid ? ArrayEngineState::Start : ArrayEngineState::Idle;
            break;
    }

    if (inputValid) {
        if (current.command.operation == CalculateMode::Add) {
            Row24 row{};
            for (unsigned col = 0; col < SauConstants::Cols; ++col) {
                row[col] = saturatingAdd24(
                    inputs.aValid ? inputs.a[col] : current.activation[col],
                    inputs.bValid ? inputs.b[col] : current.weight[col]);
            }
            next.directResults[current.inputCycles % SauConstants::Rows] = row;
        } else if (current.command.operation == CalculateMode::Transposer) {
            Row24 row{};
            for (unsigned col = 0; col < SauConstants::Cols; ++col) {
                row[col] =
                    inputs.aValid ? inputs.a[col] : current.activation[col];
            }
            next.directResults[current.inputCycles % SauConstants::Rows] = row;
        }
        next.inputCycles = current.inputCycles + 1;
    }
    if (convolution) {
        if (inputs.aLast) {
            next.completedWindows = current.completedWindows + 1;
            const uint8_t windows =
                std::max<uint8_t>(1, current.command.flowLoopTimes);
            if (current.command.registerMode != 2 ||
                next.completedWindows >= windows) {
                next.sawLast = true;
            }
        }
    } else if (inputs.aLast || inputs.bLast) {
        next.sawLast = true;
    }
    if (!inputs.start && current.sawLast && !inputValid) {
        next.drainCycles = current.drainCycles + 1;
    }

    SauPeArrayInputs peInputs;
    peInputs.enable = inputValid;
    peInputs.activations = inputs.aValid ? inputs.a : Row8{};
    peInputs.weights =
        inputs.bValid ? inputs.b : (convolution ? current.weight : Row8{});
    peInputs.bias = inputs.cValid ? inputs.c : current.bias;
    const SauCommand &peCommand =
        inputs.start && current.controller != ArrayEngineState::Work
            ? inputs.command
            : current.command;
    peInputs.instruction.operation = peCommand.operation;
    peInputs.instruction.keepMode =
        peCommand.flowMode == FlowMode::Retain ||
        peCommand.flowMode == FlowMode::Tretain;
    peInputs.instruction.shiftMode =
        current.command.shift ? ((current.inputCycles & 1) ? 3 : 2) : 0;
    if (peCommand.registerMode == 2) {
        for (unsigned col = 0; col < SauConstants::Cols; ++col) {
            peInputs.writeStrobe[col] = (current.depthwiseMask >> col) & 1;
        }
    } else {
        peInputs.writeStrobe.fill(true);
    }
    const uint16_t finishDelay =
        current.command.operation == CalculateMode::Conv ? 4 : 2;
    if (current.sawLast && current.drainCycles >= finishDelay &&
        current.drainCycles < SauConstants::Rows + finishDelay) {
        peInputs.accumulatorFinish[current.drainCycles - finishDelay] = true;
    }
    array.computeNext(peInputs);

    next.output.peFinish = arrayOut.peValid[0];

    if (arrayOut.rowValid[0]) {
        // RTL refreshes the instruction registers at OS_valid[0], even when
        // ins_valid_i arrived earlier while the engine was in WORK.
        if (current.pendingCommandValid) {
            next.command = current.pendingCommand;
            next.pendingCommandValid = false;
            next.inputCycles = 0;
            next.completedWindows = 0;
            next.drainCycles = 0;
            next.outputRow = 0;
            next.depthwiseMask = 1;
            next.sawLast = false;
            next.directResults = {};
        }
    }

    if (current.controller == ArrayEngineState::Storage &&
        !current.suppressOutput && inputs.outputReady &&
        current.outputRow < SauConstants::Rows) {
        const unsigned row = current.command.operation == CalculateMode::Add
                                 ? SauConstants::Rows - 1 - current.outputRow
                                 : current.outputRow;
        next.output.rowValid = true;
        next.output.rowIndex = current.outputRow;
        Row24 result;
        if (current.command.operation == CalculateMode::Add ||
            current.command.operation == CalculateMode::Transposer) {
            result = current.directResults[row];
        } else {
            result = arrayOut.results[row];
            if (current.command.operation == CalculateMode::Conv &&
                current.command.registerMode == 2) {
                const unsigned visibleColumns = std::min<unsigned>(
                    current.completedWindows, SauConstants::Cols);
                for (unsigned col = visibleColumns;
                     col < SauConstants::Cols; ++col) {
                    result[col] = 0;
                }
            }
        }
        next.output.row = result;
        next.output.calculateFinish =
            current.outputRow == SauConstants::Rows - 1;
        next.outputRow = current.outputRow + 1;
    }

    if (current.command.registerMode == 2 && inputs.aLast) {
        next.depthwiseMask = static_cast<uint16_t>(
            (current.depthwiseMask << 1) |
            (current.depthwiseMask >> (SauConstants::Cols - 1)));
    }
}

void
SauArrayEngine::commit()
{
    array.commit();
    current = next;
}

} // namespace gem5::sau_mikui
