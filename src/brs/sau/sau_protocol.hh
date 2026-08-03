#ifndef __BRS_SAU_SAU_PROTOCOL_HH__
#define __BRS_SAU_SAU_PROTOCOL_HH__

#include <cstdint>

#include "brs/hc/hc_protocol.hh"

namespace gem5
{
namespace brs
{

// Authoritative RTL baseline:
//   npu_lpnpu mikui_v2.0 @ 86c289c
//   hardware/src/spirit/Decoder.sv
//   hardware/src/spirit/CBU.sv
//   hardware/src/sa_element/csr.sv

enum class SauInstruction : uint8_t
{
    Unknown,
    Set1,
    Get1Lsb,
    Get1Msb,
    Set2,
    Get2Lsb,
    Get2Msb,
    Set3,
    Get3Lsb,
    Get3Msb,
    Set4,
    Get4Lsb,
    Get4Msb
};

using SauWriteType = HcWriteType;

constexpr uint32_t SauOpcode = 0x6b;
constexpr uint32_t SauFunct3 = 0x1;
constexpr uint32_t SauInstructionMask = 0xfe00707f;
constexpr uint16_t SauCsrBase = 0x200;
constexpr uint8_t SauSlotCount = 4;

using SauRequest = HcRequest;
using SauResponse = HcResponse;

constexpr bool
isSauInstruction(SauInstruction instruction)
{
    return instruction != SauInstruction::Unknown;
}

constexpr uint8_t
sauEncodingIndex(SauInstruction instruction)
{
    return isSauInstruction(instruction) ?
        static_cast<uint8_t>(instruction) - 1 : 0xff;
}

constexpr uint8_t
sauSlot(SauInstruction instruction)
{
    const uint8_t index = sauEncodingIndex(instruction);
    return index < SauSlotCount * 3 ? index / 3 + 1 : 0;
}

constexpr bool
isSauSet(SauInstruction instruction)
{
    const uint8_t index = sauEncodingIndex(instruction);
    return index < SauSlotCount * 3 && index % 3 == 0;
}

constexpr bool
isSauGetLsb(SauInstruction instruction)
{
    const uint8_t index = sauEncodingIndex(instruction);
    return index < SauSlotCount * 3 && index % 3 == 1;
}

constexpr bool
isSauGetMsb(SauInstruction instruction)
{
    const uint8_t index = sauEncodingIndex(instruction);
    return index < SauSlotCount * 3 && index % 3 == 2;
}

constexpr uint16_t
sauCsrAddress(SauInstruction instruction)
{
    const uint8_t slot = sauSlot(instruction);
    if (slot == 0) {
        return 0;
    }
    return SauCsrBase + static_cast<uint16_t>((slot - 1) * 2) +
           (isSauGetMsb(instruction) ? 1 : 0);
}

constexpr uint32_t
encodeSauInstruction(
    SauInstruction instruction,
    uint8_t rd = 0,
    uint8_t rs1 = 0,
    uint8_t rs2 = 0)
{
    const uint8_t index = sauEncodingIndex(instruction);
    return index >= SauSlotCount * 3 ? 0 :
        (static_cast<uint32_t>(index) << 25) |
        (static_cast<uint32_t>(rs2 & 0x1f) << 20) |
        (static_cast<uint32_t>(rs1 & 0x1f) << 15) |
        (SauFunct3 << 12) |
        (static_cast<uint32_t>(rd & 0x1f) << 7) |
        SauOpcode;
}

constexpr SauInstruction
decodeSauInstruction(uint32_t instruction)
{
    const uint32_t fixed =
        (SauFunct3 << 12) | SauOpcode;
    if ((instruction & SauInstructionMask) < fixed) {
        return SauInstruction::Unknown;
    }
    if ((instruction & 0x0000707f) != fixed) {
        return SauInstruction::Unknown;
    }

    const uint8_t function7 =
        static_cast<uint8_t>((instruction >> 25) & 0x7f);
    if (function7 >= SauSlotCount * 3) {
        return SauInstruction::Unknown;
    }
    return static_cast<SauInstruction>(function7 + 1);
}

constexpr bool
isSauCsr(uint16_t address)
{
    return address >= SauCsrBase &&
           address < SauCsrBase + SauSlotCount * 2;
}

constexpr uint64_t
packSauOperands(uint32_t operand1, uint32_t operand2)
{
    return packHcOperands(operand1, operand2);
}

constexpr uint32_t
unpackSauOperand1(uint64_t writeData)
{
    return unpackHcOperand1(writeData);
}

constexpr uint32_t
unpackSauOperand2(uint64_t writeData)
{
    return unpackHcOperand2(writeData);
}

static_assert(
    encodeSauInstruction(SauInstruction::Set1, 0, 14, 15) ==
        0x00f7106b,
    "MSETINS1 encoding must match the Spirit toolchain");
static_assert(
    encodeSauInstruction(SauInstruction::Get3Lsb, 8) == 0x0e00146b,
    "MGETINS3LSB encoding must match the Spirit toolchain");
static_assert(
    packSauOperands(0x11223344, 0x55667788) ==
        0x5566778811223344ULL,
    "CBU writeData must place encoded rs1 low and encoded rs2 high");
static_assert(
    sauCsrAddress(SauInstruction::Set4) == 0x206 &&
        sauCsrAddress(SauInstruction::Get4Msb) == 0x207,
    "SAU slot four must map to the final RTL CSR pair");

} // namespace brs
} // namespace gem5

#endif // __BRS_SAU_SAU_PROTOCOL_HH__
