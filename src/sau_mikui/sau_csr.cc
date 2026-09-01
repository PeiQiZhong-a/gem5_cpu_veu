#include "sau_mikui/sau_csr.hh"

namespace gem5::sau_mikui
{

namespace
{

constexpr uint16_t CsrBase = 0x200;

}

void
SauCsr::reset()
{
    current = {};
    next = current;
}

bool
SauCsr::mapped(uint16_t address)
{
    return address >= CsrBase && address < CsrBase + 8;
}

uint8_t
SauCsr::slot(uint16_t address)
{
    return static_cast<uint8_t>((address - CsrBase) >> 1);
}

bool
SauCsr::highHalf(uint16_t address)
{
    return ((address - CsrBase) & 1) != 0;
}

uint32_t
SauCsr::readSlot(const State &state, uint8_t index, bool high)
{
    const auto &command = state.command;
    uint32_t low = 0;
    uint32_t upper = 0;
    switch (index) {
        case 0:
            low = (command.registerMode & 0x3) |
                  ((command.convKernel & 0x3) << 2) |
                  (static_cast<uint32_t>(command.stride) << 4) |
                  (static_cast<uint32_t>(command.shift) << 5);
            upper = (static_cast<uint32_t>(command.transposeMode) & 0x3) |
                    (static_cast<uint32_t>(command.cutbit & 0x1f) << 2) |
                    (static_cast<uint32_t>(command.lastInstruction) << 7);
            break;
        case 1:
            low = command.verticalXStep |
                  (static_cast<uint32_t>(command.horizontalXStep) << 8) |
                  (static_cast<uint32_t>(command.outputXStep) << 16);
            upper =
                command.verticalChannelStep |
                (static_cast<uint32_t>(command.horizontalChannelStep) << 8) |
                (static_cast<uint32_t>(command.outputChannelStep) << 16);
            break;
        case 2:
            low = command.verticalBase & 0xfffff;
            upper = command.horizontalBase & 0xfffff;
            break;
        case 3:
            low = static_cast<uint32_t>(state.start) |
                  (static_cast<uint32_t>(command.instructionId) << 1) |
                  ((command.outputBase & 0xfffff) << 9) |
                  (static_cast<uint32_t>(state.busy) << 31);
            upper =
                (command.biasBase & 0xfffff) |
                ((static_cast<uint32_t>(command.operation) & 0x3) << 20) |
                ((static_cast<uint32_t>(command.flowMode) & 0x3) << 22) |
                (static_cast<uint32_t>(command.flowLoopTimes & 0x3f) << 24);
            break;
        default:
            break;
    }
    return high ? upper : low;
}

void
SauCsr::writeSlot(State &state, uint8_t index, uint8_t writeType,
                  uint64_t value)
{
    auto &command = state.command;
    switch (index) {
        case 0:
            command.registerMode = value & 0x3;
            // csr.sv assigns a two-bit slice into a three-bit register.
            command.convKernel = (value >> 2) & 0x3;
            command.reuseMode = (value >> 3) & 0x1;
            command.stride = (value >> 4) & 1;
            command.shift = (value >> 5) & 1;
            command.transposeMode =
                static_cast<TransposeMode>((value >> 32) & 0x3);
            command.cutbit = (value >> 34) & 0x1f;
            command.lastInstruction = (value >> 39) & 1;
            break;
        case 1:
            command.verticalXStep = value & 0xff;
            command.horizontalXStep = (value >> 8) & 0xff;
            command.outputXStep = (value >> 16) & 0xff;
            command.verticalChannelStep = (value >> 32) & 0xff;
            command.horizontalChannelStep = (value >> 40) & 0xff;
            command.outputChannelStep = (value >> 48) & 0xff;
            break;
        case 2:
            command.verticalBase = value & 0xfffff;
            command.horizontalBase = (value >> 32) & 0xfffff;
            break;
        case 3: {
            command.instructionId = (value >> 1) & 0xff;
            command.outputBase = (value >> 9) & 0xfffff;
            command.biasBase = (value >> 32) & 0xfffff;
            command.operation =
                static_cast<CalculateMode>((value >> 52) & 0x3);
            command.flowMode = static_cast<FlowMode>((value >> 54) & 0x3);
            command.flowLoopTimes = (value >> 56) & 0x3f;
            const bool bit = value & 1;
            const bool setTerm = (writeType & 0x1) && (state.start || bit);
            const bool clearTerm = (writeType & 0x2) && (state.start && bit);
            state.start = setTerm || clearTerm;
            break;
        }
        default:
            break;
    }
}

SauCsrOutputs
SauCsr::evaluate() const
{
    return {
        current.ready,   current.ready ? current.readData : 0,
        current.start,   current.busy,
        current.start,   current.error,
        current.command,
    };
}

void
SauCsr::computeNext(const SauCsrInputs &inputs)
{
    next = current;
    next.ready = false;
    next.readData = 0;
    next.error = current.processing && inputs.request.write &&
                 mapped(inputs.request.address);

    // These two always_ff blocks sample the same old start/flow_end values.
    if (inputs.flowEnd) {
        next.processing = false;
    } else if (current.start) {
        next.processing = true;
    }

    if (current.start || inputs.flowEnd) {
        next.busy = !inputs.flowEnd;
        next.start = false;
        next.ready = inputs.request.read;
    } else if (inputs.request.write && mapped(inputs.request.address)) {
        writeSlot(next, slot(inputs.request.address), inputs.request.writeType,
                  inputs.request.writeData);
        next.ready = true;
    } else if (inputs.request.read && mapped(inputs.request.address)) {
        next.readData = readSlot(current, slot(inputs.request.address),
                                 highHalf(inputs.request.address));
        next.ready = true;
    }
}

void
SauCsr::commit()
{
    current = next;
}

} // namespace gem5::sau_mikui
