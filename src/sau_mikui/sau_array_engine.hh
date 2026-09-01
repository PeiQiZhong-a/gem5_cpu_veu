#ifndef __SAU_MIKUI_SAU_ARRAY_ENGINE_HH__
#define __SAU_MIKUI_SAU_ARRAY_ENGINE_HH__

#include <array>
#include <cstdint>

#include "sau_mikui/sau_pe.hh"

namespace gem5::sau_mikui
{

enum class ArrayEngineState : uint8_t
{
    Idle,
    Start,
    Work,
    Storage,
    Done
};

struct SauArrayEngineInputs
{
    bool start = false;
    SauCommand command{};
    bool aValid = false;
    Row8 a{};
    bool aLast = false;
    bool bValid = false;
    Row8 b{};
    bool bLast = false;
    bool cValid = false;
    Row16 c{};
    bool outputReady = true;
};

struct SauArrayEngineOutputs
{
    ArrayEngineState state = ArrayEngineState::Idle;
    bool peFinish = false;
    bool storageReady = false;
    bool rowValid = false;
    uint8_t rowIndex = 0;
    Row24 row{};
    bool calculateFinish = false;
};

class SauArrayEngine
{
  public:
    void reset();
    SauArrayEngineOutputs evaluate() const;
    void computeNext(const SauArrayEngineInputs &inputs);
    void commit();

  private:
    struct State
    {
        ArrayEngineState controller = ArrayEngineState::Idle;
        SauCommand command{};
        Row8 activation{};
        Row8 weight{};
        Row16 bias{};
        uint16_t inputCycles = 0;
        uint8_t completedWindows = 0;
        uint16_t drainCycles = 0;
        uint8_t outputRow = 0;
        uint16_t depthwiseMask = 1;
        bool sawLast = false;
        std::array<Row24, SauConstants::Rows> directResults{};
        SauCommand pendingCommand{};
        bool pendingCommandValid = false;
        bool suppressOutput = false;
        SauArrayEngineOutputs output{};
    } current, next;

    SauPeArray array;
    static uint16_t calculateCycles(uint8_t kernel);
};

} // namespace gem5::sau_mikui

#endif // __SAU_MIKUI_SAU_ARRAY_ENGINE_HH__
