#ifndef __BRS_PIPELINE_VEU_DECODE_HH__
#define __BRS_PIPELINE_VEU_DECODE_HH__

#include <cstdint>

#include "brs/veu/veu_protocol.hh"

namespace gem5
{
namespace brs
{

struct VeuDecodeInfo
{
    bool valid = false;
    bool csrInstruction = false;
    VeuInstruction operation = VeuInstruction::Unknown;

    uint8_t rd = 0;
    uint8_t rs1 = 0;
    uint8_t rs2 = 0;
    uint8_t rs3 = 0;
    bool usesRs1 = false;
    bool usesRs2 = false;
    bool usesRs3 = false;
    bool writesRd = false;

    uint16_t csrAddr = 0;
    bool csrRead = false;
    bool csrWrite = false;
    VeuWriteType writeType = VeuWriteType::Write;
    uint32_t veStart = 0;
};

constexpr bool
isVeuCsrInstruction(VeuInstruction operation)
{
    return operation == VeuInstruction::CsrOr ||
           operation == VeuInstruction::CsrAnd ||
           operation == VeuInstruction::CsrWrite ||
           operation == VeuInstruction::CsrRead;
}

constexpr VeuDecodeInfo
decodeSpiritVeuInstruction(uint32_t instruction)
{
    VeuDecodeInfo decoded;
    decoded.operation = decodeVeuInstruction(instruction);

    if (decoded.operation == VeuInstruction::Unknown) {
        return decoded;
    }

    decoded.valid = true;
    decoded.rd = static_cast<uint8_t>((instruction >> 7) & 0x1f);
    decoded.rs1 = static_cast<uint8_t>((instruction >> 15) & 0x1f);
    decoded.rs2 = static_cast<uint8_t>((instruction >> 20) & 0x1f);
    decoded.rs3 = static_cast<uint8_t>((instruction >> 27) & 0x1f);

    if (isVeuCsrInstruction(decoded.operation)) {
        decoded.csrInstruction = true;
        decoded.usesRs1 = true;
        decoded.csrAddr =
            static_cast<uint16_t>((instruction >> 20) & 0x0fff);
        decoded.csrRead = true;

        switch (decoded.operation) {
          case VeuInstruction::CsrOr:
            decoded.writesRd = true;
            decoded.csrWrite = true;
            decoded.writeType = VeuWriteType::Set;
            break;
          case VeuInstruction::CsrAnd:
            decoded.writesRd = true;
            decoded.csrWrite = true;
            decoded.writeType = VeuWriteType::Clear;
            break;
          case VeuInstruction::CsrWrite:
            decoded.writesRd = false;
            decoded.csrWrite = true;
            decoded.writeType = VeuWriteType::Write;
            break;
          case VeuInstruction::CsrRead:
            decoded.writesRd = true;
            decoded.csrWrite = false;
            decoded.writeType = VeuWriteType::Write;
            break;
          default:
            break;
        }

        return decoded;
    }

    decoded.usesRs1 = true;
    decoded.usesRs2 = true;
    decoded.usesRs3 = isTwoShotVeuInstruction(decoded.operation);
    decoded.writesRd = true;
    decoded.csrAddr = static_cast<uint16_t>(VeuCsr::ReadAddress1);
    decoded.csrRead = true;
    decoded.csrWrite = true;
    decoded.writeType = VeuWriteType::VectorStart;
    decoded.veStart = veuStartMask(decoded.operation);
    return decoded;
}

static_assert(decodeSpiritVeuInstruction(0x0000200b).valid,
              "VSETCSR must decode as a Spirit VEU CSR instruction");
static_assert(!decodeSpiritVeuInstruction(0x0000200b).writesRd,
              "VSETCSR must not write rd");
static_assert(
    decodeSpiritVeuInstruction(0x0000000b).writeType == VeuWriteType::Set &&
    decodeSpiritVeuInstruction(0x0000000b).csrRead &&
    decodeSpiritVeuInstruction(0x0000000b).csrWrite,
    "VORCSR must perform a CSR set transaction");
static_assert(
    decodeSpiritVeuInstruction(0x0000100b).writeType == VeuWriteType::Clear &&
    decodeSpiritVeuInstruction(0x0000100b).csrRead &&
    decodeSpiritVeuInstruction(0x0000100b).csrWrite,
    "VANDCSR must perform a CSR clear transaction");
static_assert(
    decodeSpiritVeuInstruction(0x0000300b).writesRd &&
    decodeSpiritVeuInstruction(0x0000300b).csrRead &&
    !decodeSpiritVeuInstruction(0x0000300b).csrWrite,
    "VGETCSR must read the CSR and write rd");
static_assert(decodeSpiritVeuInstruction(0x0000006b).veStart == 1,
              "VADD must select vestart bit zero");
static_assert(decodeSpiritVeuInstruction(0x0200002b).usesRs3,
              "VMSUB must read the R4-format rs3 field");

} // namespace brs
} // namespace gem5

#endif // __BRS_PIPELINE_VEU_DECODE_HH__
