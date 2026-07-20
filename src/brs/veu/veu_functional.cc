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

int32_t
asSigned(uint32_t value)
{
    return static_cast<int32_t>(value);
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

} // anonymous namespace

void
VeuFunctionalExecutor::reset()
{
    reductionAccumulator = 0;
    reductionValue = 0;
    haveReductionValue = false;
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
      case VeuInstruction::Move: return "vmv";
      case VeuInstruction::ShiftRightLogical: return "vssrl";
      case VeuInstruction::ShiftRightArithmetic: return "vssra";
      case VeuInstruction::NarrowClip: return "vnclip";
      case VeuInstruction::ReduceMin: return "vredmin";
      case VeuInstruction::ReduceMax: return "vredmax";
      case VeuInstruction::ReduceSum: return "vredsum";
      case VeuInstruction::Multiply: return "vmul";
      case VeuInstruction::MultiplyAdd: return "vmadd";
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
        info.sourceMask = src1;
        info.reduction = instruction == VeuInstruction::ReduceSum ||
                         instruction == VeuInstruction::ReduceMin ||
                         instruction == VeuInstruction::ReduceMax;
        break;
      case VeuInstruction::ShiftRightLogical:
      case VeuInstruction::ShiftRightArithmetic:
      case VeuInstruction::NarrowClip:
        info.sourceMask = scalarEnabled ? src2 : (src1 | src2);
        break;
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
      case VeuInstruction::MultiplyHighSignedUnsigned:
        info.sourceMask = src1 | src2;
        break;
      case VeuInstruction::MultiplyAdd:
      case VeuInstruction::MultiplySubtract:
        info.sourceMask = src1 | src2 | src3;
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
    const bool scalarEnabled = (input.config & 0x800u) != 0;
    const uint32_t mode = (input.config >> 7) & 0x3;
    const uint32_t elementBytes = (mode & 1) ? 2 : 1;
    const uint32_t elementCount = VeuVectorBytes / elementBytes;
    const uint32_t elementMask =
        (uint32_t{1} << (elementBytes * 8)) - 1;
    const bool signedMode = (mode & 2) != 0;

    if (input.instruction == VeuInstruction::NarrowClip) {
        const int32_t minimum = signedMode ?
            signedElement(input.scalar, elementBytes) :
            static_cast<int32_t>(input.scalar & elementMask);
        const int32_t maximum = signedMode ?
            signedElement(input.scalar >> 16, elementBytes) :
            static_cast<int32_t>((input.scalar >> 16) & elementMask);
        for (uint32_t lane = 0; lane < elementCount; ++lane) {
            const uint32_t raw = loadElement(input.source2, lane, elementBytes);
            const int32_t value = signedMode ? signedElement(raw, elementBytes) :
                static_cast<int32_t>(raw);
            const int32_t clipped = value < minimum ? minimum :
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
        const uint32_t lhs = scalarEnabled &&
            input.instruction == VeuInstruction::Add ? input.scalar : a;
        const uint32_t shift = scalarEnabled ? input.scalar : a;
        uint32_t out = 0;

        switch (input.instruction) {
          case VeuInstruction::Add: out = lhs + b; break;
          case VeuInstruction::Sub: out = a - b; break;
          case VeuInstruction::Min:
            out = signedMode ?
                (signedElement(a, elementBytes) <
                    signedElement(b, elementBytes) ? a : b) :
                std::min(a, b);
            break;
          case VeuInstruction::Max:
            out = signedMode ?
                (signedElement(a, elementBytes) >
                    signedElement(b, elementBytes) ? a : b) :
                std::max(a, b);
            break;
          case VeuInstruction::And: out = a & b; break;
          case VeuInstruction::Or: out = a | b; break;
          case VeuInstruction::Xor: out = a ^ b; break;
          case VeuInstruction::Move: out = a; break;
          case VeuInstruction::ShiftRightLogical:
            out = b >> (shift & 0x1f);
            break;
          case VeuInstruction::ShiftRightArithmetic:
            out = static_cast<uint32_t>(signedElement(b, elementBytes) >>
                                        (shift & (elementBytes * 8 - 1)));
            break;
          case VeuInstruction::Multiply:
            out = static_cast<uint32_t>(static_cast<uint64_t>(a) * b);
            break;
          case VeuInstruction::MultiplyHigh:
            out = static_cast<uint32_t>((static_cast<int64_t>(asSigned(a)) *
                                         asSigned(b)) >> 32);
            break;
          case VeuInstruction::MultiplyHighSignedUnsigned:
            out = static_cast<uint32_t>((static_cast<int64_t>(asSigned(a)) *
                                         static_cast<uint64_t>(b)) >> 32);
            break;
          case VeuInstruction::MultiplyAdd:
            out = static_cast<uint32_t>(static_cast<uint64_t>(a) * b + c);
            break;
          case VeuInstruction::MultiplySubtract:
            out = static_cast<uint32_t>(static_cast<uint64_t>(a) * b - c);
            break;
          case VeuInstruction::ReduceSum:
            reductionAccumulator += signedMode ?
                signedElement(a, elementBytes) : a;
            break;
          case VeuInstruction::ReduceMin:
          case VeuInstruction::ReduceMax: {
            const bool take = !haveReductionValue ||
                (input.instruction == VeuInstruction::ReduceMin ?
                    (signedMode ? signedElement(a, elementBytes) <
                                      signedElement(reductionValue, elementBytes) :
                                  a < reductionValue) :
                    (signedMode ? signedElement(a, elementBytes) >
                                      signedElement(reductionValue, elementBytes) :
                                  a > reductionValue));
            if (take) reductionValue = a;
            haveReductionValue = true;
            break;
          }
          default:
            break;
        }
        storeElement(result.data, lane, elementBytes, out & elementMask);
    }

    if (input.instruction == VeuInstruction::ReduceSum) {
        result.writeResult = input.chunkIndex + 1 == input.chunkCount;
        if (result.writeResult) {
            storeLane(result.data, 0,
                      static_cast<uint32_t>(reductionAccumulator));
        }
    } else if (input.instruction == VeuInstruction::ReduceMin ||
               input.instruction == VeuInstruction::ReduceMax) {
        result.writeResult = input.chunkIndex + 1 == input.chunkCount;
        if (result.writeResult) {
            storeElement(result.data, 0, elementBytes, reductionValue);
        }
    }
    if (result.writeStrobe == 0) {
        result.writeResult = false;
    }
    return result;
}

} // namespace brs
} // namespace gem5
