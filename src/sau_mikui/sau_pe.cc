#include "sau_mikui/sau_pe.hh"

#include "sau_mikui/sau_constants.hh"

namespace gem5::sau_mikui
{

void
SauPe::reset()
{
    current = {};
    next = current;
}

SauPeOutputs
SauPe::evaluate() const
{
    const bool convOrAdd = current.convModeD;
    const bool valid = convOrAdd ? current.finishD2 : current.finishD1;
    return {
        current.activation,
        current.weight,
        current.writeStrobe,
        current.instruction,
        current.accumulator,
        current.result,
        valid,
    };
}

void
SauPe::computeNext(const SauPeInputs &inputs)
{
    next = current;
    const bool macEnable = current.writeStrobe && inputs.enable;
    const bool convMode = inputs.instruction.operation == CalculateMode::Conv;
    const bool convOrAdd = current.convModeD;
    const bool valid = convOrAdd ? current.finishD2 : current.finishD1;
    const bool addFlag = current.finishD1 && convOrAdd;
    const bool addStateValid = addFlag || current.macEnable[1];

    next.writeStrobe = inputs.writeStrobe;
    next.enableD = macEnable;
    next.finishD1 = inputs.accumulatorFinish;
    next.finishD2 = current.finishD1;
    next.convModeD = convMode;
    next.instruction = inputs.instruction;
    next.macEnable = {current.enableD, current.macEnable[0]};

    if (inputs.enable) {
        next.activation = inputs.activation;
        next.weight = inputs.weight;
    }

    // DW02_mult_2_stage in this source checkout has one registered PRODUCT.
    const int16_t activation = inputs.instruction.shiftMode == 2
                                   ? static_cast<uint8_t>(current.activation)
                                   : current.activation;
    next.multiply =
        wrapSigned(static_cast<int32_t>(activation) * current.weight, 18);

    if (inputs.accumulatorFinish && convMode) {
        next.augend = wrapSigned(inputs.bias, 24);
    } else if (current.instruction.shiftMode == 3) {
        const int32_t low17 = wrapSigned(current.multiply, 17);
        next.augend = wrapSigned(static_cast<int64_t>(low17) << 8, 24);
    } else {
        next.augend = wrapSigned(current.multiply, 18);
    }

    if (current.clearD) {
        next.accumulator = 0;
    } else if (addStateValid) {
        next.accumulator =
            saturatingAdd24(current.accumulator, current.augend);
    }
    if (valid) {
        next.result = current.accumulator;
    }
    next.clearD = valid && !current.instruction.keepMode;
}

void
SauPe::commit()
{
    current = next;
}

SauPeArray::SauPeArray()
{
    reset();
}

void
SauPeArray::reset()
{
    for (auto &row : pe) {
        for (auto &element : row) {
            element.reset();
        }
    }
    activationSkew = {};
    weightSkew = {};
    instructionSkew = {};
    finishSkew = {};
    enableSkew = {};
    activationSkewNext = {};
    weightSkewNext = {};
    instructionSkewNext = {};
    finishSkewNext = {};
    enableSkewNext = {};
    peValid = {};
    rowValid = {};
    rowValidOutput = {};
    peValidNext = {};
    rowValidNext = {};
    rowValidOutputNext = {};
}

SauPeArrayOutputs
SauPeArray::evaluate() const
{
    SauPeArrayOutputs outputs;
    for (unsigned row = 0; row < SauConstants::Rows; ++row) {
        for (unsigned col = 0; col < SauConstants::Cols; ++col) {
            const auto output = pe[row][col].evaluate();
            outputs.accumulators[row][col] = output.accumulator;
            outputs.results[row][col] = output.result;
            outputs.valid[row][col] = output.valid;
        }
        outputs.peValid[row] = peValid[row];
        outputs.rowValid[row] = rowValidOutput[row];
    }
    return outputs;
}

void
SauPeArray::computeNext(const SauPeArrayInputs &inputs)
{
    activationSkewNext = activationSkew;
    weightSkewNext = weightSkew;
    instructionSkewNext = instructionSkew;
    finishSkewNext = finishSkew;
    enableSkewNext = enableSkew;
    peValidNext = {};
    rowValidNext = {};
    rowValidOutputNext = rowValid;

    // SA_ROW registers the first-PE and last-PE valid indications before
    // exposing PE_valid_o and OS_valid_o to SA_ENGINE.
    for (unsigned row = 0; row < SauConstants::Rows; ++row) {
        peValidNext[row] = pe[row][0].evaluate().valid;
        rowValidNext[row] =
            pe[row][SauConstants::Cols - 1].evaluate().valid;
    }

    for (unsigned row = 0; row < SauConstants::Rows; ++row) {
        for (unsigned col = SauConstants::Cols - 1; col > 0; --col) {
            enableSkewNext[row][col] = enableSkew[row][col - 1];
        }
        enableSkewNext[row][0] =
            row == 0 ? inputs.enable : enableSkew[row - 1][0];
    }

    for (unsigned row = 0; row < SauConstants::Rows; ++row) {
        for (unsigned stage = SauConstants::Cols - 1; stage > 0; --stage) {
            finishSkewNext[row][stage] = finishSkew[row][stage - 1];
        }
        finishSkewNext[row][0] = inputs.accumulatorFinish[row];
    }

    for (unsigned row = 0; row < SauConstants::Rows; ++row) {
        for (unsigned stage = SauConstants::Rows - 1; stage > 0; --stage) {
            activationSkewNext[row][stage] = activationSkew[row][stage - 1];
        }
        activationSkewNext[row][0] =
            inputs.enable ? inputs.activations[row] : 0;
    }
    for (unsigned col = 0; col < SauConstants::Cols; ++col) {
        for (unsigned stage = SauConstants::Cols - 1; stage > 0; --stage) {
            weightSkewNext[col][stage] = weightSkew[col][stage - 1];
        }
        weightSkewNext[col][0] = inputs.enable ? inputs.weights[col] : 0;
    }
    for (unsigned row = SauConstants::Rows - 1; row > 0; --row) {
        instructionSkewNext[row] = instructionSkew[row - 1];
    }
    instructionSkewNext[0] = inputs.instruction;

    for (unsigned row = 0; row < SauConstants::Rows; ++row) {
        for (unsigned col = 0; col < SauConstants::Cols; ++col) {
            const auto above =
                row == 0 ? SauPeOutputs{} : pe[row - 1][col].evaluate();
            const auto left =
                col == 0 ? SauPeOutputs{} : pe[row][col - 1].evaluate();
            SauPeInputs peInputs;
            peInputs.enable = enableSkew[row][col];
            peInputs.writeStrobe =
                row == 0 ? inputs.writeStrobe[col] : above.writeStrobe;
            peInputs.activation =
                col == 0 ? activationSkew[row][row] : left.activation;
            peInputs.weight = row == 0 ? weightSkew[col][col] : above.weight;
            peInputs.accumulatorFinish = finishSkew[row][col];
            peInputs.instruction =
                col == 0 ? instructionSkew[row] : left.instruction;
            peInputs.bias = inputs.bias[col];
            pe[row][col].computeNext(peInputs);
        }
    }
}

void
SauPeArray::commit()
{
    for (auto &row : pe) {
        for (auto &element : row) {
            element.commit();
        }
    }
    activationSkew = activationSkewNext;
    weightSkew = weightSkewNext;
    instructionSkew = instructionSkewNext;
    finishSkew = finishSkewNext;
    enableSkew = enableSkewNext;
    peValid = peValidNext;
    rowValid = rowValidNext;
    rowValidOutput = rowValidOutputNext;
}

} // namespace gem5::sau_mikui
