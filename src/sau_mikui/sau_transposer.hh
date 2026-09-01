#ifndef __SAU_MIKUI_SAU_TRANSPOSER_HH__
#define __SAU_MIKUI_SAU_TRANSPOSER_HH__

#include <array>
#include <cstdint>

#include "sau_mikui/sau_types.hh"

namespace gem5::sau_mikui
{

struct SauTransposerInputs
{
    Row8 row{};
    bool writeEnable = false;
    bool readEnable = false;
    bool transpose = false;
    bool reuse = false;
};

struct SauTransposerOutputs
{
    Row8 row{};
    bool inputReady = true;
    bool outputReady = false;
    bool valid = false;
    bool last = false;
    bool error = false;
    uint8_t inputCount = 0;
    uint8_t outputCount = 0;
};

// Non-MODULE_TEST behavior of transposer_tiny.v.
class SauTransposer
{
  public:
    void reset();
    SauTransposerOutputs evaluate() const;
    void computeNext(const SauTransposerInputs &inputs);
    void commit();

  private:
    struct State
    {
        std::array<Row8, SauConstants::TransposerDepth> rows{};
        Row8 output{};
        uint8_t inputCount = 0;
        uint8_t outputCount = 0;
        bool inputReady = true;
        bool outputReady = false;
        bool valid = false;
        bool last = false;
        bool error = false;
    } current, next;
};

} // namespace gem5::sau_mikui

#endif // __SAU_MIKUI_SAU_TRANSPOSER_HH__
