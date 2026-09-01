#include "sau_mikui/sau_address_generator.hh"

#include <algorithm>

#include "sau_mikui/sau_constants.hh"

namespace gem5::sau_mikui
{

void
SauAddressGenerator::reset()
{
    current = {};
    current.registerFileClear = true;
    next = current;
}

uint16_t
SauAddressGenerator::executeCycles(uint8_t kernel)
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

uint16_t
SauAddressGenerator::verticalXCount(const SauCommand &command)
{
    // The RTL counter is halved for 16-bit matrix/pointwise data, but concat
    // emits the adjacent 128-bit beat for every counter value.  This model's
    // token counter represents emitted beats, so the effective count remains
    // the full execution width.
    return std::max<uint16_t>(1, executeCycles(command.convKernel));
}

uint16_t
SauAddressGenerator::verticalChannelCount(const SauCommand &command)
{
    return std::max<uint16_t>(1, command.flowLoopTimes >> command.shift);
}

uint16_t
SauAddressGenerator::horizontalXCount(const SauCommand &command)
{
    if (command.convKernel >= 3) {
        return command.registerMode == 2 ? 1 : command.convKernel;
    }
    return SauConstants::Rows;
}

uint16_t
SauAddressGenerator::horizontalChannelCount(const SauCommand &command)
{
    if (command.registerMode == 2) {
        const uint16_t count =
            command.stride
                ? ((command.flowLoopTimes << 1) >> command.shift) + 1
                : (command.flowLoopTimes >> command.shift) + 2;
        return std::max<uint16_t>(1, count);
    }
    const bool convReuse = command.convKernel >= 3;
    return std::max<uint16_t>(1, command.flowLoopTimes >>
                                     (command.shift && convReuse));
}

uint32_t
SauAddressGenerator::verticalAddress(const State &state)
{
    const auto &c = state.command;
    uint32_t x = state.vertical.x;
    if (c.shift && c.convKernel < 3 && (state.vertical.channel & 1)) {
        x += 8;
    }
    const uint32_t xResult = (x * c.verticalXStep) & 0x3fff;
    const uint32_t channelResult =
        (state.vertical.channel * c.verticalChannelStep) & 0x3fff;
    uint32_t offset;
    if (c.flowLoopTimes == (1u << c.shift)) {
        offset = ((xResult * c.verticalChannelStep) & 0xfffff) << 4;
    } else {
        offset = ((c.verticalXStep * channelResult + xResult) & 0xfffff) << 4;
    }
    return SauConstants::BaseAddress + ((c.verticalBase + offset) & 0xfffff);
}

uint32_t
SauAddressGenerator::horizontalAddress(const State &state)
{
    const auto &c = state.command;
    const bool pointwise = c.convKernel == 1;
    const uint32_t x = state.horizontal.x;
    const uint32_t channel = pointwise ? (state.horizontal.channel >> c.shift)
                                       : state.horizontal.channel;
    const uint32_t xResult = (x * c.horizontalXStep) & 0x3fff;
    const uint32_t channelResult =
        (channel * c.horizontalChannelStep) & 0x3fff;
    uint32_t offset;
    if (c.convKernel == 0) {
        offset = (channel + xResult) << 4;
    } else if ((c.flowLoopTimes == (1u << c.shift) || pointwise) &&
               c.registerMode != 2) {
        offset = ((xResult * c.horizontalChannelStep) & 0xfffff) << 4;
    } else {
        offset = ((c.horizontalXStep * channelResult + xResult) & 0xfffff)
                 << 4;
    }
    return SauConstants::BaseAddress + ((c.horizontalBase + offset) & 0xfffff);
}

uint32_t
SauAddressGenerator::outputAddress(const State &state)
{
    const auto &c = state.command;
    const uint32_t rowOffset =
        ((state.outputRow * c.outputXStep * c.outputChannelStep) & 0xfffff)
        << 4;
    const uint32_t halfOffset = state.outputHighHalf ? 16 : 0;
    return SauConstants::BaseAddress +
           ((c.outputBase + rowOffset + halfOffset) & 0xfffff);
}

bool
SauAddressGenerator::advance(Counter &counter, uint16_t xCount,
                             uint16_t channelCount)
{
    if (++counter.x < xCount) {
        return false;
    }
    counter.x = 0;
    if (++counter.channel < channelCount) {
        return false;
    }
    counter.channel = 0;
    counter.active = false;
    counter.completed = true;
    return true;
}

SauAddressOutputs
SauAddressGenerator::evaluate() const
{
    const Token &token = current.pipeline.back();
    return {
        token.address,
        token.valid && !token.write,
        token.valid && token.write,
        token.valid && !token.write && token.last,
        token.valid && token.write && token.last,
        current.rawLast,
        current.registerFileClear,
    };
}

void
SauAddressGenerator::computeNext(const SauAddressInputs &inputs)
{
    next = current;
    for (unsigned stage = SauConstants::AddressDelay - 1; stage > 0; --stage) {
        next.pipeline[stage] = current.pipeline[stage - 1];
    }
    next.pipeline[0] = {};
    next.rawLast = false;
    next.previousLastFlow = inputs.lastFlowTime;
    if (current.pipeline.back().valid && current.pipeline.back().write &&
        current.pipeline.back().last) {
        next.registerFileClear = true;
    }

    if (inputs.start) {
        next.command = inputs.command;
        next.vertical = {};
        next.horizontal = {};
        next.outputRow = 0;
        next.outputHighHalf = false;
        next.biasActive = false;
        next.biasBeat = 0;
        next.previousChooseVertical = false;
        next.registerFileClear = false;
    }

    if (inputs.lastFlowTime && !current.previousLastFlow &&
        current.command.operation == CalculateMode::Conv) {
        next.biasActive = true;
        next.biasBeat = 0;
    }

    if (inputs.writeValid) {
        next.outputActive = true;
    }
    if (current.outputActive || inputs.writeValid) {
        Token token;
        token.valid = inputs.writeValid;
        token.write = true;
        token.address = outputAddress(current);
        token.last = inputs.writeLast;
        next.pipeline[0] = token;
        if (inputs.writeValid) {
            if (current.command.shift && !current.outputHighHalf) {
                next.outputHighHalf = true;
            } else {
                next.outputHighHalf = false;
                next.outputRow = (current.outputRow + 1) % SauConstants::Rows;
            }
        }
        if (inputs.writeLast) {
            next.outputActive = false;
        }
        return;
    }

    if (current.biasActive) {
        Token token;
        token.valid = true;
        token.address = SauConstants::BaseAddress +
                        ((current.command.biasBase +
                          static_cast<uint32_t>(current.biasBeat) * 16) &
                         0xfffff);
        next.pipeline[0] = token;
        if (current.biasBeat == 1) {
            next.biasBeat = 0;
            next.biasActive = false;
        } else {
            next.biasBeat = current.biasBeat + 1;
        }
        return;
    }

    const bool active = inputs.state != SchedulerState::Idle &&
                        inputs.state != SchedulerState::DOut &&
                        !inputs.lastFlowTime;
    if (!active) {
        return;
    }

    const bool chooseVertical = (inputs.inputSwitch & 1) != 0;
    Counter &counter = chooseVertical ? next.vertical : next.horizontal;
    if (chooseVertical != current.previousChooseVertical) {
        counter.completed = false;
    }
    next.previousChooseVertical = chooseVertical;
    if (counter.completed) {
        return;
    }
    if (!counter.active) {
        counter.active = true;
        counter.x = 0;
        counter.channel = 0;
    }

    Token token;
    token.valid = true;
    token.address =
        chooseVertical ? verticalAddress(current) : horizontalAddress(current);
    bool last;
    if (chooseVertical) {
        const uint16_t xCount = verticalXCount(current.command);
        // RTL vertical_cnt_last pulses at every completed execution window;
        // channel completion only controls counter teardown.
        last = counter.x + 1 >= xCount;
        advance(counter, xCount, verticalChannelCount(current.command));
    } else {
        last = advance(counter, horizontalXCount(current.command),
                       horizontalChannelCount(current.command));
    }
    token.last = last;
    next.pipeline[0] = token;
    next.rawLast = last;
}

void
SauAddressGenerator::commit()
{
    current = next;
}

} // namespace gem5::sau_mikui
