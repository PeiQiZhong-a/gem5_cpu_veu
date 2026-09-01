#ifndef __SAU_MIKUI_SAU_OUTPUT_PATH_HH__
#define __SAU_MIKUI_SAU_OUTPUT_PATH_HH__

#include "sau_mikui/sau_types.hh"

namespace gem5::sau_mikui
{

struct SauOutputPathInputs
{
    bool valid = false;
    uint8_t rowIndex = 0;
    Row24 row{};
    bool last = false;
    uint8_t cutbit = 0;
    bool shift = false;
};

struct SauOutputPathOutputs
{
    bool valid = false;
    uint8_t rowIndex = 0;
    Row16 row{};
    bool last = false;
};

class SauOutputPath
{
  public:
    void reset();
    SauOutputPathOutputs evaluate() const;
    void computeNext(const SauOutputPathInputs &inputs);
    void commit();

  private:
    SauOutputPathOutputs current{}, next{};
};

} // namespace gem5::sau_mikui

#endif // __SAU_MIKUI_SAU_OUTPUT_PATH_HH__
