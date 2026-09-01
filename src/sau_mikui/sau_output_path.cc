#include "sau_mikui/sau_output_path.hh"

namespace gem5::sau_mikui
{

void
SauOutputPath::reset()
{
    current = {};
    next = current;
}

SauOutputPathOutputs
SauOutputPath::evaluate() const
{
    return current;
}

void
SauOutputPath::computeNext(const SauOutputPathInputs &inputs)
{
    next = {};
    next.valid = inputs.valid;
    next.rowIndex = inputs.rowIndex;
    next.last = inputs.last;
    if (inputs.valid) {
        for (unsigned col = 0; col < SauConstants::Cols; ++col) {
            next.row[col] = saturatingTruncate(inputs.row[col], inputs.cutbit,
                                               inputs.shift);
        }
    }
}

void
SauOutputPath::commit()
{
    current = next;
}

} // namespace gem5::sau_mikui
