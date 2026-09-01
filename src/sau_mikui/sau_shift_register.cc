#include "sau_mikui/sau_shift_register.hh"

#include <algorithm>

namespace gem5::sau_mikui
{

void
SauShiftRegister::reset()
{
    current = {};
    next = current;
}

SauShiftRegisterOutputs
SauShiftRegister::evaluate() const
{
    return current.output;
}

void
SauShiftRegister::computeNext(const SauShiftRegisterInputs &inputs)
{
    next = current;
    next.output = {};

    const uint8_t kernel = std::max<uint8_t>(1, inputs.kernel & 0x7);
    const bool active = current.countActive || inputs.valid;
    if (!active) {
        return;
    }

    Row8 shifted = current.shifted;
    if (current.kernelCount == 0) {
        shifted = inputs.data;
    } else {
        for (unsigned i = 0; i + 1 < SauConstants::Cols; ++i) {
            shifted[i] = current.shifted[i + 1];
        }
        const unsigned source =
            kernel > 3 ? std::min<unsigned>(current.kernelCount,
                                            SauConstants::Cols - 1)
                       : std::min<unsigned>(SauConstants::Cols - 2 +
                                                current.kernelCount,
                                            SauConstants::Cols - 1);
        shifted.back() = inputs.data[source];
    }
    next.shifted = shifted;

    if (inputs.stride) {
        if (current.kernelCount == 0) {
            for (unsigned i = 0; i < SauConstants::Cols; ++i) {
                next.strideData[i * 2] = shifted[i];
                next.strideData[i * 2 + 1] = inputs.data[i];
            }
        } else if (current.kernelCount == 1) {
            for (unsigned i = 0; i < SauConstants::Cols; ++i) {
                next.strideData[i * 2] = shifted[i];
                next.strideData[i * 2 + 1] = inputs.data[i];
            }
        } else if (current.kernelCount >= 2) {
            std::move(next.strideData.begin() + 1, next.strideData.end(),
                      next.strideData.begin());
            next.strideData.back() = inputs.data[std::min<unsigned>(
                current.kernelCount, SauConstants::Cols - 1)];
        }
        for (unsigned i = 0; i < SauConstants::Cols; ++i) {
            next.output.data[i] = next.strideData[i * 2];
        }
    } else {
        next.output.data = shifted;
    }

    next.output.valid = true;
    next.output.almostLast = current.kernelCount == kernel - 2;
    next.output.last = current.kernelCount == kernel - 1;
    if (next.output.last) {
        next.kernelCount = 0;
        next.countActive = false;
    } else {
        next.kernelCount = current.kernelCount + 1;
        next.countActive = true;
    }
    if (inputs.last) {
        next.strideData = {};
    }
}

void
SauShiftRegister::commit()
{
    current = next;
}

} // namespace gem5::sau_mikui
