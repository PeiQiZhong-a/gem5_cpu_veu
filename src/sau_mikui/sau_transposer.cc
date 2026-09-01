#include "sau_mikui/sau_transposer.hh"

namespace gem5::sau_mikui
{

void
SauTransposer::reset()
{
    current = {};
    current.inputReady = true;
    next = current;
}

SauTransposerOutputs
SauTransposer::evaluate() const
{
    return {
        current.output,     current.inputReady,  current.outputReady,
        current.valid,      current.last,        current.error,
        current.inputCount, current.outputCount,
    };
}

void
SauTransposer::computeNext(const SauTransposerInputs &inputs)
{
    next = current;
    const bool inputLast =
        current.inputCount == SauConstants::TransposerDepth - 1;
    const bool outputLast =
        current.outputCount == SauConstants::TransposerDepth - 1;

    if (current.last) {
        next.inputReady = true;
    } else if (inputLast) {
        next.inputReady = false;
    }

    if (inputLast) {
        next.outputReady = true;
    } else if (current.last && !inputs.reuse) {
        next.outputReady = false;
    }

    if (outputLast) {
        next.error = false;
    } else if (!current.inputReady && inputs.writeEnable) {
        next.error = true;
    }

    if (inputLast) {
        next.inputCount = 0;
    } else if (inputs.writeEnable) {
        next.inputCount = current.inputCount + 1;
    }

    if (outputLast) {
        next.outputCount = 0;
    } else if (inputs.readEnable) {
        next.outputCount = current.outputCount + 1;
    }

    next.valid = inputs.readEnable && !current.last;
    next.last = outputLast;

    if (inputs.writeEnable) {
        next.rows[current.inputCount] = inputs.row;
    }
    next.output.fill(0);
    if (inputs.readEnable && !current.last) {
        if (inputs.transpose) {
            for (unsigned element = 0; element < SauConstants::Rows;
                 ++element) {
                // Exact non-MODULE_TEST packed-vector order:
                // out[a] = stored[15-a][cnt_out].
                next.output[element] =
                    current.rows[SauConstants::Rows - 1 - element]
                                [current.outputCount];
            }
        } else {
            next.output = current.rows[current.outputCount];
        }
    }
}

void
SauTransposer::commit()
{
    current = next;
}

} // namespace gem5::sau_mikui
