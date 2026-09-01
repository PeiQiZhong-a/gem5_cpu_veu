#include "sau_mikui/sau_register_file.hh"

#include <algorithm>

namespace gem5::sau_mikui
{

void
SauRegisterFile::reset()
{
    current = {};
    current.readIndexHigh = SauConstants::RegisterDepth / 2;
    current.readArmed = false;
    next = current;
}

SauRegisterFileOutputs
SauRegisterFile::evaluate() const
{
    auto output = current.output;
    output.state = current.controller;
    return output;
}

RegisterFileState
SauRegisterFile::nextController(RegisterFileState state,
                                const SauRegisterFileInputs &inputs)
{
    if (inputs.clear) {
        return RegisterFileState::Idle;
    }
    switch (state) {
        case RegisterFileState::Idle:
            return inputs.writeValid && inputs.kernel >= 1
                       ? RegisterFileState::Loading
                       : RegisterFileState::Idle;
        case RegisterFileState::Loading:
            if (!inputs.writeValid) {
                return RegisterFileState::Loading;
            }
            if (inputs.shift) {
                return RegisterFileState::Shifting;
            }
            if (inputs.stride) {
                return RegisterFileState::Striding;
            }
            return RegisterFileState::Padding;
        case RegisterFileState::Padding:
            return inputs.writeValid ? RegisterFileState::Loading
                                     : RegisterFileState::Storing;
        case RegisterFileState::Shifting:
            return inputs.stride && inputs.writeValid
                       ? RegisterFileState::Striding
                       : RegisterFileState::Padding;
        case RegisterFileState::Striding:
            return inputs.shift && inputs.writeValid
                       ? RegisterFileState::StrShift
                       : RegisterFileState::Padding;
        case RegisterFileState::StrShift:
            return inputs.writeValid ? RegisterFileState::Padding
                                     : RegisterFileState::StrShift;
        case RegisterFileState::Storing:
            return inputs.writeValid ? RegisterFileState::Loading
                                     : RegisterFileState::Storing;
    }
    return RegisterFileState::Idle;
}

void
SauRegisterFile::computeNext(const SauRegisterFileInputs &inputs)
{
    next = current;
    next.output = {};
    next.controller = nextController(current.controller, inputs);

    if (inputs.clear) {
        next.writeIndex = 0;
        next.validEntries = 0;
        next.readIndexLow = 0;
        next.readIndexHigh = SauConstants::RegisterDepth / 2;
        next.outputCount = 0;
        next.highHalf = false;
        next.readArmed = false;
        return;
    }

    if (inputs.writeValid) {
        Beat128 masked{};
        for (unsigned i = 0; i < masked.size(); ++i) {
            masked[i] = (inputs.dataMask >> i) & 1 ? inputs.writeData[i] : 0;
        }
        Entry low{};
        for (unsigned i = 0; i < SauConstants::Rows; ++i) {
            low[i] = static_cast<int8_t>(masked[i]);
        }
        low[SauConstants::Rows] = static_cast<int8_t>(masked[0]);
        low[SauConstants::Rows + 1] = static_cast<int8_t>(masked[1]);
        next.storage[current.writeIndex] = low;

        if (inputs.shift) {
            Entry high{};
            for (unsigned i = 0; i < SauConstants::Rows; ++i) {
                const unsigned byte = i * 2 + 1;
                high[i] = static_cast<int8_t>(
                    byte < masked.size()
                        ? masked[byte]
                        : current.previousBeat[byte - masked.size()]);
            }
            high[SauConstants::Rows] = static_cast<int8_t>(masked[2]);
            high[SauConstants::Rows + 1] = static_cast<int8_t>(masked[3]);
            const unsigned highIndex =
                (current.writeIndex + SauConstants::RegisterDepth / 2) %
                SauConstants::RegisterDepth;
            next.storage[highIndex] = high;
        }
        next.previousBeat = masked;
        next.writeIndex =
            (current.writeIndex + 1) % SauConstants::RegisterDepth;
        next.validEntries = std::min<uint8_t>(SauConstants::RegisterDepth,
                                              current.validEntries + 1);
    }

    if (inputs.readEnable) {
        next.readArmed = true;
    }
    if (current.readArmed) {
        const uint8_t kernel = std::max<uint8_t>(1, inputs.kernel & 0x7);
        const uint8_t extension =
            (inputs.kernel >= 2 ? 1 : 0) + (inputs.stride ? 1 : 0);
        const uint8_t cycles = std::max<uint8_t>(1, kernel * (extension + 1));
        const uint8_t index = inputs.shift && current.highHalf
                                  ? current.readIndexHigh
                                  : current.readIndexLow;
        const Entry &entry = current.storage[index];
        const unsigned extra =
            inputs.kernel <= 3 && current.outputCount >= kernel ? 2 : 0;
        for (unsigned i = 0; i < SauConstants::Rows; ++i) {
            next.output.data[i] =
                entry[std::min<unsigned>(i + extra, EntryBytes - 1)];
        }
        next.output.valid = true;
        next.output.last = current.outputCount == cycles - 1;

        if (next.output.last) {
            next.outputCount = 0;
            next.readArmed = inputs.readEnable;
            if (inputs.shift) {
                next.highHalf = !current.highHalf;
            }
        } else {
            next.outputCount = current.outputCount + 1;
            if (inputs.shift && current.highHalf) {
                next.readIndexHigh =
                    (current.readIndexHigh + 1) % SauConstants::RegisterDepth;
            } else {
                const uint8_t depth =
                    std::max<uint8_t>(1, current.validEntries);
                next.readIndexLow = (current.readIndexLow + 1) % depth;
            }
        }
    }
}

void
SauRegisterFile::commit()
{
    current = next;
}

} // namespace gem5::sau_mikui
