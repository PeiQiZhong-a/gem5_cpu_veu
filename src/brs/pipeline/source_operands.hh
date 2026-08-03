#ifndef __BRS_PIPELINE_SOURCE_OPERANDS_HH__
#define __BRS_PIPELINE_SOURCE_OPERANDS_HH__

#include <cstdint>

#include "brs/pipeline/sau_decode.hh"
#include "brs/pipeline/veu_decode.hh"

namespace gem5
{

struct SourceOperands
{
    uint8_t rs1 = 0;
    uint8_t rs2 = 0;
    uint8_t rs3 = 0;
    bool usesRs1 = false;
    bool usesRs2 = false;
    bool usesRs3 = false;
};

constexpr SourceOperands
decodeSourceOperands(uint32_t instruction)
{
    SourceOperands sources;
    sources.rs1 = static_cast<uint8_t>((instruction >> 15) & 0x1f);
    sources.rs2 = static_cast<uint8_t>((instruction >> 20) & 0x1f);
    sources.rs3 = static_cast<uint8_t>((instruction >> 27) & 0x1f);

    const brs::SauDecodeInfo sau =
        brs::decodeSpiritSauInstruction(instruction);
    if (sau.valid) {
        sources.rs1 = sau.rs1;
        sources.rs2 = sau.rs2;
        sources.usesRs1 = sau.usesRs1;
        sources.usesRs2 = sau.usesRs2;
        return sources;
    }

    const brs::VeuDecodeInfo veu =
        brs::decodeSpiritVeuInstruction(instruction);
    if (veu.valid) {
        sources.rs1 = veu.rs1;
        sources.rs2 = veu.rs2;
        sources.rs3 = veu.rs3;
        sources.usesRs1 = veu.usesRs1;
        sources.usesRs2 = veu.usesRs2;
        sources.usesRs3 = veu.usesRs3;
        return sources;
    }

    switch (instruction & 0x7f) {
      case 0x13: // I-type ALU
      case 0x03: // loads
      case 0x67: // JALR
        sources.usesRs1 = true;
        break;

      case 0x23: // stores
      case 0x33: // R-type ALU
      case 0x63: // branches
        sources.usesRs1 = true;
        sources.usesRs2 = true;
        break;

      default:
        break;
    }

    return sources;
}

static_assert(decodeSourceOperands(0x00510093).usesRs1,
              "ADDI must use rs1");
static_assert(!decodeSourceOperands(0x00510093).usesRs2,
              "ADDI immediate bits must not be treated as rs2");
static_assert(decodeSourceOperands(0x0000006b).usesRs2,
              "VADD must use rs2");
static_assert(decodeSourceOperands(0x0200002b).usesRs3,
              "VMSUB must use rs3");
static_assert(
    decodeSourceOperands(
        brs::encodeSauInstruction(brs::SauInstruction::Get4Msb, 1, 2, 3))
        .usesRs1,
    "Spirit enables rs1 even for MGET instructions");
static_assert(
    decodeSourceOperands(
        brs::encodeSauInstruction(brs::SauInstruction::Get4Msb, 1, 2, 3))
        .usesRs2,
    "Spirit enables rs2 even for MGET instructions");

} // namespace gem5

#endif // __BRS_PIPELINE_SOURCE_OPERANDS_HH__
