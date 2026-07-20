#ifndef __BRS_VEU_VEU_FUNCTIONAL_HH__
#define __BRS_VEU_VEU_FUNCTIONAL_HH__

#include <array>
#include <cstdint>
#include <string>

#include "brs/veu/veu_protocol.hh"

namespace gem5
{
namespace brs
{

using VeuVector = std::array<uint8_t, VeuVectorBytes>;

enum class VeuSource : uint8_t
{
    None = 0,
    Source1 = 1,
    Source2 = 2,
    Source3 = 3
};

struct VeuOperationInfo
{
    VeuInstruction instruction = VeuInstruction::Unknown;
    const char *name = "illegal";
    uint8_t sourceMask = 0;
    bool reduction = false;
    bool supported = false;
};

struct VeuFunctionalInput
{
    VeuInstruction instruction = VeuInstruction::Unknown;
    uint32_t config = 0;
    uint32_t scalar = 0;
    uint32_t writeMask = 0xffffffffu;
    uint32_t chunkIndex = 0;
    uint32_t chunkCount = 1;
    VeuVector source1 = {};
    VeuVector source2 = {};
    VeuVector source3 = {};
};

struct VeuFunctionalResult
{
    VeuVector data = {};
    uint32_t writeStrobe = 0xffffffffu;
    bool writeResult = true;
};

class VeuFunctionalExecutor
{
  public:
    void reset();
    VeuFunctionalResult execute(const VeuFunctionalInput &input);

    static VeuOperationInfo describe(VeuInstruction instruction,
                                     bool scalarEnabled);
    static const char *instructionName(VeuInstruction instruction);
    static bool sourceRequired(uint8_t mask, VeuSource source);

  private:
    int64_t reductionAccumulator = 0;
    uint32_t reductionValue = 0;
    bool haveReductionValue = false;
};

} // namespace brs
} // namespace gem5

#endif
