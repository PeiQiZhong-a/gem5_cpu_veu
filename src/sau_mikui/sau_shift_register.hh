#ifndef __SAU_MIKUI_SAU_SHIFT_REGISTER_HH__
#define __SAU_MIKUI_SAU_SHIFT_REGISTER_HH__

#include <array>
#include <cstdint>

#include "sau_mikui/sau_types.hh"

namespace gem5::sau_mikui
{

struct SauShiftRegisterInputs
{
    bool valid = false;
    Row8 data{};
    bool last = false;
    uint8_t kernel = 0;
    bool stride = false;
};

struct SauShiftRegisterOutputs
{
    bool valid = false;
    Row8 data{};
    bool last = false;
    bool almostLast = false;
};

class SauShiftRegister
{
  public:
    void reset();
    SauShiftRegisterOutputs evaluate() const;
    void computeNext(const SauShiftRegisterInputs &inputs);
    void commit();

  private:
    struct State
    {
        uint8_t kernelCount = 0;
        bool countActive = false;
        Row8 shifted{};
        std::array<int8_t, SauConstants::Cols * 2> strideData{};
        SauShiftRegisterOutputs output{};
    } current, next;
};

} // namespace gem5::sau_mikui

#endif // __SAU_MIKUI_SAU_SHIFT_REGISTER_HH__
