#ifndef __SAU_MIKUI_SAU_ADDRESS_GENERATOR_HH__
#define __SAU_MIKUI_SAU_ADDRESS_GENERATOR_HH__

#include <array>
#include <cstdint>

#include "sau_mikui/sau_types.hh"

namespace gem5::sau_mikui
{

struct SauAddressInputs
{
    bool start = false;
    SchedulerState state = SchedulerState::Idle;
    uint8_t inputSwitch = 0;
    bool lastFlowTime = false;
    bool writeValid = false;
    bool writeLast = false;
    SauCommand command;
};

struct SauAddressOutputs
{
    uint32_t address = 0;
    bool readEnable = false;
    bool writeEnable = false;
    bool readLast = false;
    bool writeLast = false;
    bool loadDone = false;
    bool registerFileClear = true;
};

class SauAddressGenerator
{
  public:
    void reset();
    SauAddressOutputs evaluate() const;
    void computeNext(const SauAddressInputs &inputs);
    void commit();

  private:
    struct Token
    {
        uint32_t address = 0;
        bool valid = false;
        bool write = false;
        bool last = false;
    };
    struct Counter
    {
        uint16_t x = 0;
        uint16_t channel = 0;
        bool active = false;
        bool completed = false;
    };
    struct State
    {
        SauCommand command;
        Counter vertical;
        Counter horizontal;
        uint8_t outputRow = 0;
        bool outputHighHalf = false;
        bool outputActive = false;
        bool previousChooseVertical = false;
        bool previousLastFlow = false;
        bool biasActive = false;
        uint8_t biasBeat = 0;
        std::array<Token, SauConstants::AddressDelay> pipeline{};
        bool rawLast = false;
        bool registerFileClear = true;
    } current, next;

    static uint16_t executeCycles(uint8_t kernel);
    static uint16_t verticalXCount(const SauCommand &command);
    static uint16_t verticalChannelCount(const SauCommand &command);
    static uint16_t horizontalXCount(const SauCommand &command);
    static uint16_t horizontalChannelCount(const SauCommand &command);
    static uint32_t verticalAddress(const State &state);
    static uint32_t horizontalAddress(const State &state);
    static uint32_t outputAddress(const State &state);
    static bool advance(Counter &counter, uint16_t xCount,
                        uint16_t channelCount);
};

} // namespace gem5::sau_mikui

#endif // __SAU_MIKUI_SAU_ADDRESS_GENERATOR_HH__
