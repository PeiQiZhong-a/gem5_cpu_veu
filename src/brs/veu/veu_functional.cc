#include "brs/veu/veu_functional.hh"

#include <algorithm>
#include <limits>

namespace gem5
{
namespace brs
{

namespace
{

void
storeLane(VeuVector &data, uint32_t lane, uint32_t value)
{
    const uint32_t offset = lane * sizeof(uint32_t);
    data[offset] = value & 0xff;
    data[offset + 1] = (value >> 8) & 0xff;
    data[offset + 2] = (value >> 16) & 0xff;
    data[offset + 3] = (value >> 24) & 0xff;
}

uint32_t
loadElement(const VeuVector &data, uint32_t index, uint32_t bytes)
{
    uint32_t value = 0;
    for (uint32_t byte = 0; byte < bytes; ++byte) {
        value |= static_cast<uint32_t>(data[index * bytes + byte]) <<
                 (byte * 8);
    }
    return value;
}

void
storeElement(VeuVector &data, uint32_t index, uint32_t bytes, uint32_t value)
{
    for (uint32_t byte = 0; byte < bytes; ++byte) {
        data[index * bytes + byte] = (value >> (byte * 8)) & 0xff;
    }
}

int32_t
signedElement(uint32_t value, uint32_t bytes)
{
    if (bytes == 1) return static_cast<int8_t>(value);
    if (bytes == 2) return static_cast<int16_t>(value);
    return static_cast<int32_t>(value);
}

uint32_t
scalarElement(uint32_t scalar, uint32_t index, uint32_t bytes)
{
    const uint32_t elementsPerWord = sizeof(uint32_t) / bytes;
    const uint32_t wordElement = index % elementsPerWord;
    const uint32_t bits = bytes * 8;
    const uint32_t mask = (uint32_t{1} << bits) - 1;
    return (scalar >> (wordElement * bits)) & mask;
}

int64_t
elementValue(uint32_t value, uint32_t bytes, bool signedMode)
{
    return signedMode ? signedElement(value, bytes) :
                        static_cast<int64_t>(value);
}

uint32_t
saturateElement(int64_t value, uint32_t bytes, bool signedMode)
{
    if (signedMode) {
        const int64_t minimum = bytes == 1 ?
            std::numeric_limits<int8_t>::min() :
            std::numeric_limits<int16_t>::min();
        const int64_t maximum = bytes == 1 ?
            std::numeric_limits<int8_t>::max() :
            std::numeric_limits<int16_t>::max();
        value = std::clamp(value, minimum, maximum);
    } else {
        const int64_t maximum = bytes == 1 ?
            std::numeric_limits<uint8_t>::max() :
            std::numeric_limits<uint16_t>::max();
        value = std::clamp<int64_t>(value, 0, maximum);
    }
    return static_cast<uint32_t>(value);
}

uint32_t
loadWord(const VeuVector &data, uint32_t word)
{
    return loadElement(data, word, sizeof(uint32_t));
}

VeuVector
shiftChunkUp(const VeuVector &current, const VeuVector &previous,
             uint32_t bytes)
{
    VeuVector output = {};
    if (bytes == 0) return current;
    for (uint32_t index = 0; index < VeuVectorBytes; ++index) {
        output[index] = index >= bytes ?
            current[index - bytes] :
            previous[VeuVectorBytes - bytes + index];
    }
    return output;
}

VeuVector
shiftChunkDown(const VeuVector &current, const VeuVector &next,
               uint32_t bytes)
{
    VeuVector output = {};
    if (bytes == 0) return current;
    for (uint32_t index = 0; index < VeuVectorBytes; ++index) {
        output[index] = index + bytes < VeuVectorBytes ?
            current[index + bytes] :
            next[index + bytes - VeuVectorBytes];
    }
    return output;
}

} // anonymous namespace

void
VeuFunctionalExecutor::reset()
{
    reductionAccumulator = 0;
    reductionValue = 0;
    haveReductionValue = false;
    slidePrevious = {};
    haveSlidePrevious = false;
}

const char *
VeuFunctionalExecutor::instructionName(VeuInstruction instruction)
{
    switch (instruction) {
      case VeuInstruction::Add: return "vadd";
      case VeuInstruction::Sub: return "vsub";
      case VeuInstruction::Min: return "vmin";
      case VeuInstruction::Max: return "vmax";
      case VeuInstruction::And: return "vand";
      case VeuInstruction::Or: return "vor";
      case VeuInstruction::Xor: return "vxor";
      case VeuInstruction::SlideUp: return "vslideup";
      case VeuInstruction::SlideDown: return "vslidedown";
      case VeuInstruction::Move: return "vmv";
      case VeuInstruction::ShiftRightLogical: return "vssrl";
      case VeuInstruction::ShiftRightArithmetic: return "vssra";
      case VeuInstruction::NarrowClip: return "vnclip";
      case VeuInstruction::ReduceMin: return "vredmin";
      case VeuInstruction::ReduceMax: return "vredmax";
      case VeuInstruction::ReduceSum: return "vredsum";
      case VeuInstruction::WidenReduceSum: return "vwredsum";
      case VeuInstruction::Compress: return "vcompress";
      case VeuInstruction::Multiply: return "vmul";
      case VeuInstruction::MultiplyAdd: return "vmac";
      case VeuInstruction::MultiplySubtract: return "vmsub";
      case VeuInstruction::MultiplyHigh: return "vmulh";
      case VeuInstruction::MultiplyHighSignedUnsigned: return "vmulhsu";
      default: return "illegal";
    }
}

VeuOperationInfo
VeuFunctionalExecutor::describe(VeuInstruction instruction,
                                bool scalarEnabled)
{
    VeuOperationInfo info;
    info.instruction = instruction;
    info.name = instructionName(instruction);
    info.supported = true;

    const uint8_t src1 = uint8_t{1} << 0;
    const uint8_t src2 = uint8_t{1} << 1;
    const uint8_t src3 = uint8_t{1} << 2;
    switch (instruction) {
      case VeuInstruction::Move:
      case VeuInstruction::ReduceSum:
      case VeuInstruction::ReduceMin:
      case VeuInstruction::ReduceMax:
        info.sourceMask = scalarEnabled ? 0 : src1;
        info.reduction = instruction == VeuInstruction::ReduceSum ||
                         instruction == VeuInstruction::ReduceMin ||
                         instruction == VeuInstruction::ReduceMax;
        break;
      case VeuInstruction::WidenReduceSum:
        info.sourceMask = scalarEnabled ? src2 : (src1 | src2);
        info.rtlIllegal = true;
        break;
      case VeuInstruction::SlideUp:
      case VeuInstruction::SlideDown:
      case VeuInstruction::ShiftRightLogical:
      case VeuInstruction::ShiftRightArithmetic:
      case VeuInstruction::NarrowClip:
      case VeuInstruction::Add:
        info.sourceMask = scalarEnabled ? src2 : (src1 | src2);
        break;
      case VeuInstruction::Sub:
      case VeuInstruction::Min:
      case VeuInstruction::Max:
      case VeuInstruction::And:
      case VeuInstruction::Or:
      case VeuInstruction::Xor:
      case VeuInstruction::Multiply:
      case VeuInstruction::MultiplyHigh:
        info.sourceMask = scalarEnabled ? src2 : (src1 | src2);
        info.rtlIllegal =
            instruction == VeuInstruction::MultiplyHigh;
        break;
      case VeuInstruction::MultiplyAdd:
      case VeuInstruction::MultiplySubtract:
        info.sourceMask = scalarEnabled ? (src2 | src3) :
                                          (src1 | src2 | src3);
        break;
      case VeuInstruction::Compress:
      case VeuInstruction::MultiplyHighSignedUnsigned:
        info.sourceMask = 0;
        info.supported = false;
        info.rtlIllegal = true;
        break;
      default:
        info.sourceMask = 0;
        info.supported = false;
        break;
    }
    return info;
}

bool
VeuFunctionalExecutor::sourceRequired(uint8_t mask, VeuSource source)
{
    if (source == VeuSource::None) {
        return false;
    }
    return mask & (uint8_t{1} << (static_cast<uint8_t>(source) - 1));
}

VeuFunctionalResult
VeuFunctionalExecutor::execute(const VeuFunctionalInput &input)
{
    VeuFunctionalResult result;
    result.writeStrobe = input.writeMask;
    result.outputChunk = input.chunkIndex;
    const bool scalarEnabled = (input.config & 0x800u) != 0;
    const uint32_t mode = (input.config >> 7) & 0x3;
    const uint32_t shiftFig = (input.config >> 1) & 0x1f;
    const uint32_t elementBytes = (mode & 1) ? 2 : 1;
    const uint32_t elementCount = VeuVectorBytes / elementBytes;
    const uint32_t elementMask =
        (uint32_t{1} << (elementBytes * 8)) - 1;
    const bool signedMode = (mode & 2) != 0;

    if (input.instruction == VeuInstruction::SlideUp ||
        input.instruction == VeuInstruction::SlideDown) {
        if (!scalarEnabled) {
            result.data = input.source1;
            return result;
        }
        const uint32_t shiftBytes = input.scalar & 0x1f;
        if (input.instruction == VeuInstruction::SlideUp) {
            result.data = shiftChunkUp(input.source2, slidePrevious,
                                       shiftBytes);
            slidePrevious = input.source2;
            haveSlidePrevious = true;
            return result;
        }

        if (input.chunkCount == 1) {
            result.data = shiftChunkDown(input.source2, {}, shiftBytes);
            return result;
        }
        if (!haveSlidePrevious) {
            slidePrevious = input.source2;
            haveSlidePrevious = true;
            result.writeResult = false;
            return result;
        }
        result.outputChunk = input.chunkIndex - 1;
        result.data = shiftChunkDown(slidePrevious, input.source2,
                                     shiftBytes);
        slidePrevious = input.source2;
        if (input.chunkIndex + 1 == input.chunkCount) {
            result.hasExtraResult = true;
            result.extraChunk = input.chunkIndex;
            result.extraData = shiftChunkDown(input.source2, {}, shiftBytes);
        }
        return result;
    }

    if (input.instruction == VeuInstruction::NarrowClip) {
        for (uint32_t lane = 0; lane < elementCount; ++lane) {
            const uint32_t word = lane * elementBytes / sizeof(uint32_t);
            const uint32_t bounds = scalarEnabled ?
                input.scalar : loadWord(input.source1, word);
            const uint32_t rawMin = elementBytes == 1 ?
                (bounds & 0xff) : (bounds & 0xffff);
            const uint32_t rawMax = elementBytes == 1 ?
                ((bounds >> 16) & 0xff) : (bounds >> 16);
            const int64_t minimum =
                elementValue(rawMin, elementBytes, signedMode);
            const int64_t maximum =
                elementValue(rawMax, elementBytes, signedMode);
            const uint32_t raw = loadElement(input.source2, lane, elementBytes);
            const int64_t value =
                elementValue(raw, elementBytes, signedMode);
            const int64_t clipped = value < minimum ? minimum :
                (value > maximum ? maximum : value);
            storeElement(result.data, lane, elementBytes,
                         static_cast<uint32_t>(clipped) & elementMask);
        }
        if (result.writeStrobe == 0) result.writeResult = false;
        return result;
    }

    for (uint32_t lane = 0; lane < elementCount; ++lane) {
        const uint32_t a = loadElement(input.source1, lane, elementBytes);
        const uint32_t b = loadElement(input.source2, lane, elementBytes);
        const uint32_t c = loadElement(input.source3, lane, elementBytes);
        const uint32_t lhs = scalarEnabled ?
            scalarElement(input.scalar, lane, elementBytes) : a;
        const uint32_t word = lane * elementBytes / sizeof(uint32_t);
        const uint32_t shiftWord = scalarEnabled ?
            input.scalar : loadWord(input.source1, word);
        const uint32_t shift = shiftWord &
            (elementBytes == 1 ? 0xfu : 0x1fu);
        uint32_t out = 0;

        switch (input.instruction) {
          case VeuInstruction::Add:
            out = saturateElement(
                elementValue(lhs, elementBytes, signedMode) +
                elementValue(b, elementBytes, signedMode),
                elementBytes, signedMode);
            break;
          case VeuInstruction::Sub:
            out = saturateElement(
                elementValue(lhs, elementBytes, signedMode) -
                elementValue(b, elementBytes, signedMode),
                elementBytes, signedMode);
            break;
          case VeuInstruction::Min:
            out = signedMode ?
                (signedElement(lhs, elementBytes) <
                    signedElement(b, elementBytes) ? lhs : b) :
                std::min(lhs, b);
            break;
          case VeuInstruction::Max:
            out = signedMode ?
                (signedElement(lhs, elementBytes) >
                    signedElement(b, elementBytes) ? lhs : b) :
                std::max(lhs, b);
            break;
          case VeuInstruction::And: out = lhs & b; break;
          case VeuInstruction::Or: out = lhs | b; break;
          case VeuInstruction::Xor: out = lhs ^ b; break;
          case VeuInstruction::Move: out = lhs; break;
          case VeuInstruction::ShiftRightLogical:
            out = b >> (shift & 0x1f);
            break;
          case VeuInstruction::ShiftRightArithmetic:
            out = signedMode ?
                static_cast<uint32_t>(
                    signedElement(b, elementBytes) >> shift) :
                b >> shift;
            break;
          case VeuInstruction::Multiply:
          case VeuInstruction::MultiplyHigh:
            out = saturateElement(
                (elementValue(lhs, elementBytes, signedMode) *
                 elementValue(b, elementBytes, signedMode)) >> shiftFig,
                elementBytes, signedMode);
            break;
          case VeuInstruction::MultiplyAdd:
          case VeuInstruction::MultiplySubtract:
            out = saturateElement(
                (elementValue(lhs, elementBytes, signedMode) *
                     elementValue(b, elementBytes, signedMode) +
                 (input.instruction == VeuInstruction::MultiplySubtract ?
                      -elementValue(c, elementBytes, signedMode) :
                      elementValue(c, elementBytes, signedMode))) >> shiftFig,
                elementBytes, signedMode);
            break;
          case VeuInstruction::ReduceSum:
          case VeuInstruction::WidenReduceSum:
            reductionAccumulator += static_cast<uint32_t>(
                elementValue(lhs, elementBytes, signedMode));
            break;
          case VeuInstruction::ReduceMin:
          case VeuInstruction::ReduceMax: {
            const bool take = !haveReductionValue ||
                (input.instruction == VeuInstruction::ReduceMin ?
                    (signedMode ? signedElement(lhs, elementBytes) <
                                      signedElement(reductionValue, elementBytes) :
                                  lhs < reductionValue) :
                    (signedMode ? signedElement(lhs, elementBytes) >
                                      signedElement(reductionValue, elementBytes) :
                                  lhs > reductionValue));
            if (take) reductionValue = lhs;
            haveReductionValue = true;
            break;
          }
          default:
            break;
        }
        storeElement(result.data, lane, elementBytes, out & elementMask);
    }

    if (input.instruction == VeuInstruction::ReduceSum ||
        input.instruction == VeuInstruction::WidenReduceSum) {
        storeLane(result.data, 0, reductionAccumulator);
    }
    if (input.instruction == VeuInstruction::ReduceSum) {
        result.writeResult = input.chunkIndex + 1 == input.chunkCount;
    } else if (input.instruction == VeuInstruction::ReduceMin ||
               input.instruction == VeuInstruction::ReduceMax) {
        storeElement(result.data, 0, elementBytes, reductionValue);
        result.writeResult =
            input.chunkCount == 2 ||
            input.chunkIndex + 1 == input.chunkCount;
    }
    if (result.writeStrobe == 0) {
        result.writeResult = false;
    }
    return result;
}

} // namespace brs
} // namespace gem5
