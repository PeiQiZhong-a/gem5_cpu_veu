#include "sau_n/sau_model.hh"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace gem5::sau_n
{
namespace
{

void
requireAccumulator24(int32_t value, const char *field)
{
    if (value < Accumulator24Min || value > Accumulator24Max) {
        throw std::invalid_argument(
            std::string(field) + " must be a signed 24-bit value");
    }
}

} // anonymous namespace

int16_t
multiplySignedInt8(int8_t activation, int8_t weight)
{
    return static_cast<int16_t>(activation) * static_cast<int16_t>(weight);
}

int32_t
saturatingAddSigned24(int32_t accumulator, int32_t addend)
{
    requireAccumulator24(accumulator, "accumulator");
    requireAccumulator24(addend, "addend");
    const int64_t sum = static_cast<int64_t>(accumulator) + addend;
    return static_cast<int32_t>(std::clamp(
        sum,
        static_cast<int64_t>(Accumulator24Min),
        static_cast<int64_t>(Accumulator24Max)));
}

int8_t
quantizeSignedInt8(int32_t accumulator, uint64_t cutbit)
{
    requireAccumulator24(accumulator, "accumulator");
    if (cutbit > SauMaxCutbit) {
        throw std::invalid_argument("cutbit must be in [0, 23]");
    }
    const int64_t divisor = int64_t{1} << cutbit;
    const int64_t shifted = accumulator >= 0 ?
        accumulator / divisor :
        -((-static_cast<int64_t>(accumulator) + divisor - 1) / divisor);
    return static_cast<int8_t>(std::clamp<int64_t>(shifted, -128, 127));
}

uint16_t
signExtendedInt8Slot(int8_t value)
{
    const int32_t signedValue = value;
    return static_cast<uint16_t>(
        signedValue >= 0 ? signedValue : (1 << 16) + signedValue);
}

void
SauNumericCore::begin(uint64_t validRows, uint64_t validColumns)
{
    if (validRows == 0 || validRows > SauRows) {
        throw std::invalid_argument("valid rows must be in [1, 16]");
    }
    if (validColumns == 0 || validColumns > SauColumns) {
        throw std::invalid_argument("valid columns must be in [1, 16]");
    }
    clear();
    rows = validRows;
    columns = validColumns;
    numericPhase = SauNumericPhase::Accumulating;
    for (uint64_t row = 0; row < rows; ++row) {
        for (uint64_t column = 0; column < columns; ++column) {
            states[peIndex(row, column)].valid = true;
        }
    }
}

void
SauNumericCore::macStep(
    const Int8Lanes &activations, const Int8Lanes &weights)
{
    if (numericPhase != SauNumericPhase::Accumulating) {
        throw std::logic_error("MAC step requires accumulating phase");
    }
    for (uint64_t row = 0; row < rows; ++row) {
        for (uint64_t column = 0; column < columns; ++column) {
            auto &state = states[peIndex(row, column)];
            state.activation = activations[row];
            state.weight = weights[column];
            state.product = multiplySignedInt8(
                state.activation, state.weight);
            state.accumulator = saturatingAddSigned24(
                state.accumulator, state.product);
        }
    }
    ++completedMacSteps;
}

void
SauNumericCore::addBias(const BiasLanes &biases)
{
    if (numericPhase != SauNumericPhase::Accumulating) {
        throw std::logic_error("bias requires accumulating phase");
    }
    if (completedMacSteps == 0) {
        throw std::logic_error("bias requires at least one MAC step");
    }
    for (uint64_t row = 0; row < rows; ++row) {
        for (uint64_t column = 0; column < columns; ++column) {
            auto &state = states[peIndex(row, column)];
            state.accumulator = saturatingAddSigned24(
                state.accumulator, biases[column]);
            state.biasApplied = true;
        }
    }
    numericPhase = SauNumericPhase::Finalized;
}

SauNumericCore::Outputs
SauNumericCore::outputSnapshot(uint64_t cutbit) const
{
    if (numericPhase != SauNumericPhase::Finalized) {
        throw std::logic_error("output requires finalized phase");
    }
    Outputs outputs{};
    for (uint64_t row = 0; row < rows; ++row) {
        for (uint64_t column = 0; column < columns; ++column) {
            const auto index = peIndex(row, column);
            const auto value = quantizeSignedInt8(
                states[index].accumulator, cutbit);
            outputs[index] = {
                states[index].accumulator,
                value,
                signExtendedInt8Slot(value),
                true,
            };
        }
    }
    return outputs;
}

void
SauNumericCore::clear()
{
    states = {};
    numericPhase = SauNumericPhase::Empty;
    rows = 0;
    columns = 0;
    completedMacSteps = 0;
}

void
SauNumericCore::reset()
{
    clear();
}

const SauPeNumericState &
SauNumericCore::pe(uint64_t row, uint64_t column) const
{
    return states[peIndex(row, column)];
}

std::string_view
sauEngineStateName(SauEngineState state)
{
    switch (state) {
      case SauEngineState::Idle:
        return "IDLE";
      case SauEngineState::Start:
        return "STREAM";
      case SauEngineState::Work:
        return "DRAIN";
      case SauEngineState::Storage:
        return "BIAS";
      case SauEngineState::Done:
        return "OUTPUT";
    }
    throw std::invalid_argument("invalid SA engine state value");
}

bool
peMaskBit(const SauPeMask &mask, uint64_t row, uint64_t column)
{
    const auto index = peIndex(row, column);
    return (mask[index / 64] >> (index % 64)) & uint64_t{1};
}

SauCycleModel::SauCycleModel(SauInputProtocol protocol)
    : inputProtocol(protocol)
{
    if (inputProtocol != SauInputProtocol::StrictRtlContinuous &&
        inputProtocol != SauInputProtocol::ElasticBubbleEnabled) {
        throw std::invalid_argument("invalid SA input protocol");
    }
}

void
SauCycleModel::validateConfig(const SauCycleConfig &candidate) const
{
    if (candidate.calcCycles == 0 || candidate.calcCycles > 567) {
        throw std::invalid_argument("CALC_CYCLE must be in [1, 567]");
    }
    if (candidate.validRows == 0 || candidate.validRows > SauRows) {
        throw std::invalid_argument("valid rows must be in [1, 16]");
    }
    if (candidate.validColumns == 0 ||
        candidate.validColumns > SauColumns) {
        throw std::invalid_argument("valid columns must be in [1, 16]");
    }
    if (candidate.cutbit > SauMaxCutbit) {
        throw std::invalid_argument("cutbit must be in [0, 23]");
    }
}

void
SauCycleModel::setMaskBit(
    SauPeMask &mask, uint64_t row, uint64_t column) const
{
    const auto index = peIndex(row, column);
    mask[index / 64] |= uint64_t{1} << (index % 64);
}

void
SauCycleModel::scheduleInput(const SauCycleInputs &inputs)
{
    for (uint64_t row = 0; row < activeConfig.validRows; ++row) {
        for (uint64_t column = 0;
             column < activeConfig.validColumns; ++column) {
            uint64_t due = checkedAdd(
                currentCycle, ArrayMacCommitDelay,
                "scheduled MAC cycle");
            due = checkedAdd(due, row, "scheduled MAC cycle");
            due = checkedAdd(due, column, "scheduled MAC cycle");
            scheduledMacs[due].push_back({
                row,
                column,
                inputs.activations[row],
                inputs.weights[column],
            });
        }
    }
}

void
SauCycleModel::scheduleCompletion()
{
    for (uint64_t row = 0; row < activeConfig.validRows; ++row) {
        for (uint64_t column = 0;
             column < activeConfig.validColumns; ++column) {
            const uint64_t due = checkedAdd(
                currentCycle, ArrayDrainToBiasDelay,
                "scheduled bias cycle");
            scheduledBiases[due].push_back(
                {row, column, activeConfig.biases[column]});
        }

        const uint64_t rowDue = checkedAdd(
            currentCycle, ArrayDrainToBiasDelay,
            "scheduled row result cycle");
        scheduledRowsReady[rowDue].push_back(row);
        if (row == 0) {
            storageReadyCycle = rowDue;
        }
    }
}

SauCycleObservation
SauCycleModel::tick(const SauCycleInputs &inputs)
{
    SauCycleObservation observation;
    observation.cycle = currentCycle;

    const SauEngineState oldState = engineState;
    const uint64_t oldOutputRow = outputRow;
    if (clearRowReadyNext) {
        rowReady = {};
        clearRowReadyNext = false;
    }
    const auto macs = scheduledMacs.find(currentCycle);
    if (macs != scheduledMacs.end()) {
        for (const auto &event : macs->second) {
            const auto index = peIndex(event.row, event.column);
            auto &state = cyclePeStates[index];
            state.activation = event.activation;
            state.weight = event.weight;
            state.accumulator = saturatingAddSigned24(
                state.accumulator,
                multiplySignedInt8(event.activation, event.weight));
            setMaskBit(
                observation.peValidMask, event.row, event.column);
            setMaskBit(
                observation.macCommitMask, event.row, event.column);
        }
        scheduledMacs.erase(macs);
    }

    const auto biases = scheduledBiases.find(currentCycle);
    if (biases != scheduledBiases.end()) {
        for (const auto &event : biases->second) {
            const auto index = peIndex(event.row, event.column);
            cyclePeStates[index].accumulator = saturatingAddSigned24(
                cyclePeStates[index].accumulator, event.bias);
            setMaskBit(
                observation.addCommitMask, event.row, event.column);
        }
        scheduledBiases.erase(biases);
    }

    const auto readyRows = scheduledRowsReady.find(currentCycle);
    if (readyRows != scheduledRowsReady.end()) {
        for (const auto row : readyRows->second) {
            for (uint64_t column = 0;
                 column < activeConfig.validColumns; ++column) {
                const auto value = quantizeSignedInt8(
                    cyclePeStates[peIndex(row, column)].accumulator,
                    activeConfig.cutbit);
                rowOutputs[row][column] = signExtendedInt8Slot(value);
            }
        }
        scheduledRowsReady.erase(readyRows);
    }

    if (currentCycle == storageReadyCycle) {
        storageReady = true;
        storageReadyCycle = std::numeric_limits<uint64_t>::max();
    }
    if (currentCycle == calFinishCycle) {
        observation.calFinish = true;
        calFinishCycle = std::numeric_limits<uint64_t>::max();
        storageReady = false;
        clearRowReadyNext = true;
    }

    bool launched = false;
    if (inputs.insValid) {
        if (oldState != SauEngineState::Idle) {
            throw std::logic_error("instruction launch requires IDLE state");
        }
        if (inputs.inputValid) {
            throw std::logic_error(
                "instruction launch and first input use separate cycles");
        }
        validateConfig(inputs.config);
        activeConfig = inputs.config;
        configLoaded = true;
        acceptedInputs = 0;
        storageReady = false;
        outputRow = 0;
        rowReady = {};
        rowOutputs = {};
        cyclePeStates = {};
        launched = true;
    }

    SauEngineState nextState = oldState;
    switch (oldState) {
      case SauEngineState::Idle:
        nextState = inputs.inputValid ?
            SauEngineState::Start : SauEngineState::Idle;
        break;
      case SauEngineState::Start:
        if (!inputs.inputValid && configLoaded &&
            acceptedInputs == activeConfig.calcCycles) {
            nextState = SauEngineState::Work;
        } else if (
            inputProtocol == SauInputProtocol::StrictRtlContinuous &&
            !inputs.inputValid && acceptedInputs == 0) {
            nextState = SauEngineState::Idle;
        } else {
            nextState = SauEngineState::Start;
        }
        break;
      case SauEngineState::Work:
        nextState = currentCycle + 1 == storageReadyCycle ?
            SauEngineState::Storage : SauEngineState::Work;
        break;
      case SauEngineState::Storage:
        nextState = storageReady ?
            SauEngineState::Done : SauEngineState::Storage;
        break;
      case SauEngineState::Done:
        nextState = observation.calFinish ?
            SauEngineState::Idle : SauEngineState::Done;
        break;
    }

    if (inputs.inputValid) {
        if (!configLoaded) {
            throw std::logic_error("input requires a latched instruction");
        }
        if ((oldState != SauEngineState::Idle &&
             oldState != SauEngineState::Start) || launched) {
            throw std::logic_error("input requires IDLE/START stream state");
        }
        if (acceptedInputs >= activeConfig.calcCycles) {
            throw std::logic_error("input exceeds configured CALC_CYCLE");
        }
        scheduleInput(inputs);
        acceptedInputs = checkedAdd(
            acceptedInputs, 1, "accepted SA input count");
        if (acceptedInputs == activeConfig.calcCycles) {
            scheduleCompletion();
        }
    } else if (
        inputProtocol == SauInputProtocol::StrictRtlContinuous &&
        oldState == SauEngineState::Start && !launched &&
        acceptedInputs < activeConfig.calcCycles) {
        throw std::logic_error("SA input stream cannot contain bubbles");
    }

    const bool outputDesired = storageReady;
    bool presentedRow = false;
    uint64_t presentedRowIndex = 0;
    if (storageReady && outputRow < activeConfig.validRows) {
        presentedRow = true;
        presentedRowIndex = outputRow;
        observation.osValidMask = uint16_t{1} << outputRow;
    }
    observation.peFinish = storageReady && outputRow == 0;
    observation.internalOutputValid =
        outputDesired && inputs.outputGrant && storageReady &&
        outputRow < activeConfig.validRows;
    observation.engineOutputFire = observation.internalOutputValid;
    observation.outputCounter = launched ? oldOutputRow : outputRow;
    if (observation.engineOutputFire) {
        const bool last = outputRow + 1 == activeConfig.validRows;
        observation.rowScoreValid = true;
        observation.rowSequence = outputRow;
        observation.outputSlots = rowOutputs[outputRow];
        if (last) {
            calFinishCycle = checkedAdd(
                currentCycle, 1, "registered cal-finish cycle");
        } else {
            ++outputRow;
        }
    }

    engineState = nextState;
    observation.state = engineState;
    observation.storageReady = storageReady;
    for (uint64_t row = 0; row < SauRows; ++row) {
        if (rowReady[row]) {
            observation.rowReadyMask |= uint16_t{1} << row;
        }
    }
    if (presentedRow) {
        rowReady[presentedRowIndex] = true;
    }
    observation.dataInCount = inputs.inputValid && acceptedInputs != 0 ?
        acceptedInputs - 1 : acceptedInputs;
    if (observation.calFinish) {
        acceptedInputs = 0;
    }
    observation.peStates = cyclePeStates;
    ++currentCycle;
    return observation;
}

void
SauCycleModel::reset()
{
    currentCycle = 0;
    engineState = SauEngineState::Idle;
    activeConfig = {};
    configLoaded = false;
    acceptedInputs = 0;
    storageReady = false;
    clearRowReadyNext = false;
    outputRow = 0;
    rowReady = {};
    rowOutputs = {};
    cyclePeStates = {};
    scheduledMacs.clear();
    scheduledBiases.clear();
    scheduledRowsReady.clear();
    storageReadyCycle = std::numeric_limits<uint64_t>::max();
    calFinishCycle = std::numeric_limits<uint64_t>::max();
}

} // namespace gem5::sau_n
