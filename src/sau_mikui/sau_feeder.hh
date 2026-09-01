#ifndef __SAU_MIKUI_SAU_FEEDER_HH__
#define __SAU_MIKUI_SAU_FEEDER_HH__

#include <array>
#include <cstdint>

#include "sau_mikui/sau_types.hh"

namespace gem5::sau_mikui
{

enum class FeederInputState : uint8_t
{
    NoInput,
    OneInput,
    TwoInput
};

struct SauFeederInputs
{
    bool start = false;
    SauCommand command{};
    SchedulerState schedulerState = SchedulerState::Idle;
    uint8_t inputSwitch = 0;
    bool lastFlowTime = false;

    bool memoryValid = false;
    Beat128 memoryData{};
    bool memoryLast = false;

    bool registerValid = false;
    Row8 registerData{};
    bool registerLast = false;

    bool resultValid = false;
    bool resultIndexValid = false;
    uint8_t resultRowIndex = 0;
    Row16 resultData{};
    bool resultLast = false;
    bool executeDone = false;
};

struct SauFeederOutputs
{
    bool registerWriteValid = false;
    Beat128 registerWriteData{};
    bool registerReadEnable = false;

    bool aValid = false;
    Row8 a{};
    bool aLast = false;
    bool bValid = false;
    Row8 b{};
    bool bLast = false;
    bool cValid = false;
    Row16 c{};
    uint8_t inputSwitch = 0;

    bool writeAddressValid = false;
    bool writeAddressLast = false;
    bool writeDataValid = false;
    Beat128 writeData{};
    bool writeDataLast = false;
    bool updateFinished = false;
    bool lastFlowTimeClear = false;
    FeederInputState inputState = FeederInputState::NoInput;
};

class SauFeeder
{
  public:
    void reset();
    SauFeederOutputs evaluate() const;
    void computeNext(const SauFeederInputs &inputs);
    void commit();

  private:
    static constexpr unsigned LabelDelay = SauConstants::FeederStateDelay;

    struct Label
    {
        SchedulerState state = SchedulerState::Idle;
        uint8_t inputSwitch = 0;
        bool lastFlowTime = false;
    };

    struct State
    {
        SauCommand command{};
        std::array<Label, LabelDelay + 1> labels{};
        FeederInputState inputState = FeederInputState::NoInput;
        bool memoryValidD = false;
        bool memoryLastD = false;
        Beat128 memoryDataD{};
        bool lastFlowD = false;
        bool registerLoaded = false;
        uint8_t registerReadWindows = 0;
        uint8_t shiftCount = 0;
        bool convWindowActive = false;
        uint8_t convWindowCount = 0;
        Row8 convWindowData{};

        std::array<Row16, SauConstants::Rows> outputBuffer{};
        uint8_t outputWriteRow = 0;
        bool outputBufferValid = false;
        bool writeAddressActive = false;
        uint8_t writeAddressCount = 0;
        std::array<bool, SauConstants::AddressDelay> writeStartPipeline{};
        bool writeDataActive = false;
        uint8_t writeDataCount = 0;
        SauFeederOutputs output{};
    } current, next;

    static Row8 toRow8(const Beat128 &beat);
    static Row16 toBias(const Beat128 &currentBeat,
                        const Beat128 &previousBeat, bool depthwise);
    static Beat128 outputBeat(const State &state, uint8_t count);
};

} // namespace gem5::sau_mikui

#endif // __SAU_MIKUI_SAU_FEEDER_HH__
