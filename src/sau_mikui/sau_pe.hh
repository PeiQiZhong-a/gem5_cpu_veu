#ifndef __SAU_MIKUI_SAU_PE_HH__
#define __SAU_MIKUI_SAU_PE_HH__

#include <array>
#include <cstdint>

#include "sau_mikui/sau_types.hh"

namespace gem5::sau_mikui
{

struct SauPeInputs
{
    bool enable = false;
    bool writeStrobe = true;
    int8_t activation = 0;
    int8_t weight = 0;
    bool accumulatorFinish = false;
    PeInstruction instruction;
    int16_t bias = 0;
};

struct SauPeOutputs
{
    int8_t activation = 0;
    int8_t weight = 0;
    bool writeStrobe = false;
    PeInstruction instruction;
    int32_t accumulator = 0;
    int32_t result = 0;
    bool valid = false;
};

// State held by one SA_PE. Input row/column skew is owned by SauPeArray;
// multiply, valid, instruction, propagation and accumulator registers remain
// distinct in every one of the 256 instances.
class SauPe
{
  public:
    void reset();
    SauPeOutputs evaluate() const;
    void computeNext(const SauPeInputs &inputs);
    void commit();

  private:
    struct State
    {
        int8_t activation = 0;
        int8_t weight = 0;
        bool writeStrobe = false;
        bool enableD = false;
        bool convModeD = false;
        PeInstruction instruction;
        std::array<bool, 2> macEnable{};
        bool finishD1 = false;
        bool finishD2 = false;
        int32_t multiply = 0;
        int32_t augend = 0;
        int32_t accumulator = 0;
        int32_t result = 0;
        bool clearD = false;
    } current, next;
};

struct SauPeArrayInputs
{
    bool enable = false;
    std::array<bool, SauConstants::Cols> writeStrobe{};
    Row8 activations{};
    Row8 weights{};
    std::array<bool, SauConstants::Rows> accumulatorFinish{};
    PeInstruction instruction;
    Row16 bias{};
};

struct SauPeArrayOutputs
{
    std::array<Row24, SauConstants::Rows> accumulators{};
    std::array<Row24, SauConstants::Rows> results{};
    std::array<std::array<bool, SauConstants::Cols>, SauConstants::Rows>
        valid{};
    std::array<bool, SauConstants::Rows> peValid{};
    std::array<bool, SauConstants::Rows> rowValid{};
};

class SauPeArray
{
  public:
    SauPeArray();
    void reset();
    SauPeArrayOutputs evaluate() const;
    void computeNext(const SauPeArrayInputs &inputs);
    void commit();

  private:
    std::array<std::array<SauPe, SauConstants::Cols>, SauConstants::Rows> pe;
    std::array<std::array<int8_t, SauConstants::Rows>, SauConstants::Rows>
        activationSkew{};
    std::array<std::array<int8_t, SauConstants::Cols>, SauConstants::Cols>
        weightSkew{};
    std::array<PeInstruction, SauConstants::Rows> instructionSkew{};
    std::array<std::array<bool, SauConstants::Cols>, SauConstants::Rows>
        finishSkew{};
    std::array<std::array<bool, SauConstants::Cols>, SauConstants::Rows>
        enableSkew{};
    std::array<std::array<int8_t, SauConstants::Rows>, SauConstants::Rows>
        activationSkewNext{};
    std::array<std::array<int8_t, SauConstants::Cols>, SauConstants::Cols>
        weightSkewNext{};
    std::array<PeInstruction, SauConstants::Rows> instructionSkewNext{};
    std::array<std::array<bool, SauConstants::Cols>, SauConstants::Rows>
        finishSkewNext{};
    std::array<std::array<bool, SauConstants::Cols>, SauConstants::Rows>
        enableSkewNext{};
    std::array<bool, SauConstants::Rows> peValid{};
    std::array<bool, SauConstants::Rows> rowValid{};
    std::array<bool, SauConstants::Rows> rowValidOutput{};
    std::array<bool, SauConstants::Rows> peValidNext{};
    std::array<bool, SauConstants::Rows> rowValidNext{};
    std::array<bool, SauConstants::Rows> rowValidOutputNext{};
};

} // namespace gem5::sau_mikui

#endif // __SAU_MIKUI_SAU_PE_HH__
