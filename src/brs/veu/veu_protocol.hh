#ifndef __BRS_VEU_PROTOCOL_HH__
#define __BRS_VEU_PROTOCOL_HH__

#include <array>
#include <cstdint>

#include "brs/hc/hc_protocol.hh"

namespace gem5
{
namespace brs
{

// Authoritative sources:
//   Mikui hardware/src/veu/{VEU,VE_CORE,VCU,VLU,VFU}.sv
//   The VCU consumes one 128-bit SRAM beat and advances addresses by 0x10.

enum class VeuCsr : uint16_t
{
    Status = 0x100,
    ReadAddress1 = 0x101,
    ReadAddress2 = 0x102,
    WriteAddress = 0x103,
    Config = 0x104,
    VectorLength = 0x105,
    Mask = 0x106,
    ReadAddress3 = 0x107
};

using VeuWriteType = HcWriteType;

enum class VeuInstruction : uint8_t
{
    Unknown,
    CsrOr,
    CsrAnd,
    CsrWrite,
    CsrRead,
    Add,
    Sub,
    Min,
    Max,
    ReduceMin,
    ReduceMax,
    And,
    Or,
    Xor,
    SlideUp,
    SlideDown,
    Move,
    ShiftRightLogical,
    ShiftRightArithmetic,
    NarrowClip,
    WidenReduceSum,
    ReduceSum,
    Compress,
    MultiplySubtract,
    MultiplyAdd,
    Multiply,
    MultiplyHighSignedUnsigned,
    MultiplyHigh
};

constexpr uint32_t VeuVectorBits = 128;
constexpr uint32_t VeuVectorBytes = VeuVectorBits / 8;
constexpr uint32_t VeuLaneBits = 32;
constexpr uint32_t VeuLaneCount = VeuVectorBits / VeuLaneBits;
constexpr uint32_t VeuFullWriteMask =
    (uint32_t{1} << VeuVectorBytes) - 1;
constexpr uint32_t VeuComputeDelayCycles = 3;
constexpr uint32_t VeuTcmDelayCycles = 3;
constexpr uint32_t VeuLoadReturnLatencyCycles = VeuTcmDelayCycles + 1;

constexpr uint32_t VeuCsrOpcode = 0x0b;
constexpr uint32_t VeuVectorOpcode = 0x6b;
constexpr uint32_t VeuThreeSourceOpcode = 0x2b;
constexpr uint32_t VeuCsrInstructionMask = 0x0000707f;
constexpr uint32_t VeuVectorInstructionMask = 0xfe00707f;
constexpr uint32_t VeuThreeSourceInstructionMask = 0x0600707f;

using VeuRequest = HcRequest;
using VeuResponse = HcResponse;

// VEU-to-TCM signals exposed by the Mikui RTL VEU wrapper. Each request is
// exactly one 128-bit beat (four 32-bit words).
struct VeuMemoryRequest
{
    bool valid = false;
    uint32_t address = 0;
    uint16_t writeStrobe = 0;
    std::array<uint32_t, 4> writeData{};

    bool isWrite() const { return writeStrobe != 0; }
};

struct VeuMemoryOutput
{
    VeuMemoryRequest request;
    bool lockStart = false;
    bool lockFinish = false;
};

struct VeuMemoryResponse
{
    // The RTL return port has fixed timing and no valid signal. valid is kept
    // here so cycle models and test endpoints can identify the return edge.
    bool valid = false;
    std::array<uint32_t, 4> readData{};
    bool lockActive = false;
};

constexpr bool
isVeuCsr(uint16_t address)
{
    return address >= static_cast<uint16_t>(VeuCsr::Status) &&
           address <= static_cast<uint16_t>(VeuCsr::ReadAddress3);
}

constexpr bool
isVeuWriteType(uint8_t writeType)
{
    return writeType <= static_cast<uint8_t>(VeuWriteType::VectorStart);
}

constexpr uint64_t
packVeuOperands(uint32_t operand1, uint32_t operand2)
{
    return packHcOperands(operand1, operand2);
}

constexpr uint32_t
unpackVeuOperand1(uint64_t writeData)
{
    return unpackHcOperand1(writeData);
}

constexpr uint32_t
unpackVeuOperand2(uint64_t writeData)
{
    return unpackHcOperand2(writeData);
}

constexpr uint32_t
alignVeuLengthBits(uint32_t lengthBits)
{
    return (lengthBits + VeuVectorBits - 1) &
           ~(VeuVectorBits - 1);
}

constexpr uint32_t
effectiveVeuLengthAtStart(uint32_t configuredLengthBits)
{
    return configuredLengthBits == 0 ? 0 :
           alignVeuLengthBits(configuredLengthBits);
}

constexpr uint32_t
makeVeuVectorInstructionMatch(uint8_t function7)
{
    return (static_cast<uint32_t>(function7) << 25) | VeuVectorOpcode;
}

constexpr VeuInstruction
decodeVeuInstruction(uint32_t instruction)
{
    switch (instruction & VeuCsrInstructionMask) {
      case 0x0000000b:
        return VeuInstruction::CsrOr;
      case 0x0000100b:
        return VeuInstruction::CsrAnd;
      case 0x0000200b:
        return VeuInstruction::CsrWrite;
      case 0x0000300b:
        return VeuInstruction::CsrRead;
      default:
        break;
    }

    switch (instruction & VeuVectorInstructionMask) {
      case makeVeuVectorInstructionMatch(0x00):
        return VeuInstruction::Add;
      case makeVeuVectorInstructionMatch(0x01):
        return VeuInstruction::Sub;
      case makeVeuVectorInstructionMatch(0x02):
        return VeuInstruction::Min;
      case makeVeuVectorInstructionMatch(0x03):
        return VeuInstruction::Max;
      case makeVeuVectorInstructionMatch(0x04):
        return VeuInstruction::ReduceMin;
      case makeVeuVectorInstructionMatch(0x05):
        return VeuInstruction::ReduceMax;
      case makeVeuVectorInstructionMatch(0x06):
        return VeuInstruction::And;
      case makeVeuVectorInstructionMatch(0x07):
        return VeuInstruction::Or;
      case makeVeuVectorInstructionMatch(0x08):
        return VeuInstruction::Xor;
      case makeVeuVectorInstructionMatch(0x09):
        return VeuInstruction::SlideUp;
      case makeVeuVectorInstructionMatch(0x0a):
        return VeuInstruction::SlideDown;
      case makeVeuVectorInstructionMatch(0x0b):
        return VeuInstruction::Move;
      case makeVeuVectorInstructionMatch(0x0c):
        return VeuInstruction::ShiftRightLogical;
      case makeVeuVectorInstructionMatch(0x0d):
        return VeuInstruction::ShiftRightArithmetic;
      case makeVeuVectorInstructionMatch(0x0e):
        return VeuInstruction::NarrowClip;
      case makeVeuVectorInstructionMatch(0x0f):
        return VeuInstruction::WidenReduceSum;
      case makeVeuVectorInstructionMatch(0x10):
        return VeuInstruction::ReduceSum;
      case makeVeuVectorInstructionMatch(0x11):
        return VeuInstruction::Compress;
      case makeVeuVectorInstructionMatch(0x14):
        return VeuInstruction::Multiply;
      case makeVeuVectorInstructionMatch(0x15):
        return VeuInstruction::MultiplyHighSignedUnsigned;
      case makeVeuVectorInstructionMatch(0x16):
        return VeuInstruction::MultiplyHigh;
      default:
        break;
    }

    switch (instruction & VeuThreeSourceInstructionMask) {
      case 0x0200002b:
        return VeuInstruction::MultiplySubtract;
      case 0x0200102b:
        return VeuInstruction::MultiplyAdd;
      default:
        return VeuInstruction::Unknown;
    }
}

constexpr uint8_t
veuStartBit(VeuInstruction instruction)
{
    switch (instruction) {
      case VeuInstruction::Add: return 0;
      case VeuInstruction::Sub: return 1;
      case VeuInstruction::Min: return 2;
      case VeuInstruction::Max: return 3;
      case VeuInstruction::ReduceMin: return 4;
      case VeuInstruction::ReduceMax: return 5;
      case VeuInstruction::And: return 6;
      case VeuInstruction::Or: return 7;
      case VeuInstruction::Xor: return 8;
      case VeuInstruction::SlideUp: return 9;
      case VeuInstruction::SlideDown: return 10;
      case VeuInstruction::Move: return 11;
      case VeuInstruction::ShiftRightLogical: return 12;
      case VeuInstruction::ShiftRightArithmetic: return 13;
      case VeuInstruction::NarrowClip: return 14;
      case VeuInstruction::WidenReduceSum: return 15;
      case VeuInstruction::ReduceSum: return 16;
      case VeuInstruction::Compress: return 17;
      case VeuInstruction::MultiplySubtract: return 18;
      case VeuInstruction::MultiplyAdd: return 19;
      case VeuInstruction::Multiply: return 20;
      case VeuInstruction::MultiplyHighSignedUnsigned: return 21;
      case VeuInstruction::MultiplyHigh: return 22;
      default: return 0xff;
    }
}

constexpr uint32_t
veuStartMask(VeuInstruction instruction)
{
    const uint8_t bit = veuStartBit(instruction);
    return bit < 32 ? (uint32_t{1} << bit) : 0;
}

constexpr bool
isTwoShotVeuInstruction(VeuInstruction instruction)
{
    return instruction == VeuInstruction::MultiplyAdd ||
           instruction == VeuInstruction::MultiplySubtract;
}

static_assert(VeuVectorBytes == 16, "Mikui VEU uses 16-byte SRAM beats");
static_assert(VeuLaneCount == 4, "Mikui VEU exposes four 32-bit lanes");
static_assert(alignVeuLengthBits(129) == 256,
              "Mikui VCU rounds VEUVLEN to 128-bit boundaries");
static_assert(decodeVeuInstruction(0x0000200b) ==
              VeuInstruction::CsrWrite,
              "Spirit VSETCSR encoding changed");
static_assert(decodeVeuInstruction(0x0000006b) ==
              VeuInstruction::Add,
              "Spirit VADD encoding changed");
static_assert(decodeVeuInstruction(0x0200002b) ==
              VeuInstruction::MultiplySubtract,
              "Spirit VMSUB encoding changed");
static_assert(decodeVeuInstruction(0x0200102b) ==
              VeuInstruction::MultiplyAdd,
              "Spirit VMADD encoding changed");
static_assert(veuStartMask(VeuInstruction::Multiply) == (uint32_t{1} << 20),
              "Spirit VMUL vestart mapping changed");

} // namespace brs
} // namespace gem5

#endif // __BRS_VEU_PROTOCOL_HH__
