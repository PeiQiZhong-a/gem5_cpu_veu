#ifndef __BRS_PIPELINE_SAU_DECODE_HH__
#define __BRS_PIPELINE_SAU_DECODE_HH__

#include <cstdint>

#include "brs/sau/sau_protocol.hh"

namespace gem5
{
namespace brs
{

struct SauDecodeInfo
{
    bool valid = false;
    SauInstruction operation = SauInstruction::Unknown;
    uint8_t slot = 0;

    uint8_t rd = 0;
    uint8_t rs1 = 0;
    uint8_t rs2 = 0;
    bool usesRs1 = false;
    bool usesRs2 = false;
    bool writesRd = false;

    uint16_t csrAddr = 0;
    bool csrRead = false;
    bool csrWrite = false;
    SauWriteType writeType = SauWriteType::Write;
    uint32_t veStart = 0;
};

constexpr SauDecodeInfo
decodeSpiritSauInstruction(uint32_t instruction)
{
    SauDecodeInfo decoded;
    decoded.operation = decodeSauInstruction(instruction);
    if (!isSauInstruction(decoded.operation)) {
        return decoded;
    }

    decoded.valid = true;
    decoded.slot = sauSlot(decoded.operation);
    decoded.rd = static_cast<uint8_t>((instruction >> 7) & 0x1f);
    decoded.rs1 = static_cast<uint8_t>((instruction >> 15) & 0x1f);
    decoded.rs2 = static_cast<uint8_t>((instruction >> 20) & 0x1f);

    // Decoder.scala enables both source ports and rd_we for every one of
    // the 21 SAU encodings. MGET normally encodes x0 for both sources and
    // MSET normally encodes x0 for rd, but arbitrary encodings still carry
    // these architectural dependencies in the RTL.
    decoded.usesRs1 = true;
    decoded.usesRs2 = true;
    decoded.writesRd = true;

    decoded.csrAddr = sauCsrAddress(decoded.operation);
    decoded.csrRead = !isSauSet(decoded.operation);
    decoded.csrWrite = isSauSet(decoded.operation);
    decoded.writeType = decoded.csrWrite ?
        SauWriteType::Set : SauWriteType::Clear;
    decoded.veStart = 0;
    return decoded;
}

static_assert(
    decodeSpiritSauInstruction(0x00f7106b).operation ==
        SauInstruction::Set1,
    "Spirit MSETINS1 must decode");
static_assert(
    decodeSpiritSauInstruction(0x0e00146b).operation ==
        SauInstruction::Get3Lsb,
    "Spirit MGETINS3LSB must decode");
static_assert(
    decodeSpiritSauInstruction(0x0e00146b).csrAddr == 0x204,
    "MGETINS3LSB must address SAU CSR 0x204");

} // namespace brs
} // namespace gem5

#endif // __BRS_PIPELINE_SAU_DECODE_HH__
