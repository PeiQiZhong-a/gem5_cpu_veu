#ifndef __SAU_MIKUI_SAU_REGISTER_FILE_HH__
#define __SAU_MIKUI_SAU_REGISTER_FILE_HH__

#include <array>
#include <cstdint>

#include "sau_mikui/sau_types.hh"

namespace gem5::sau_mikui
{

enum class RegisterFileState : uint8_t
{
    Idle,
    Loading,
    Padding,
    Shifting,
    Striding,
    StrShift,
    Storing
};

struct SauRegisterFileInputs
{
    bool writeValid = false;
    Beat128 writeData{};
    uint16_t dataMask = 0xffff;
    bool readEnable = false;
    bool clear = false;
    uint8_t kernel = 0;
    uint8_t registerMode = 0;
    bool stride = false;
    bool shift = false;
};

struct SauRegisterFileOutputs
{
    bool valid = false;
    Row8 data{};
    bool last = false;
    RegisterFileState state = RegisterFileState::Idle;
};

class SauRegisterFile
{
  public:
    void reset();
    SauRegisterFileOutputs evaluate() const;
    void computeNext(const SauRegisterFileInputs &inputs);
    void commit();

  private:
    static constexpr unsigned EntryBytes = SauConstants::Rows + 2;
    using Entry = std::array<int8_t, EntryBytes>;

    struct State
    {
        RegisterFileState controller = RegisterFileState::Idle;
        std::array<Entry, SauConstants::RegisterDepth> storage{};
        uint8_t writeIndex = 0;
        uint8_t validEntries = 0;
        uint8_t readIndexLow = 0;
        uint8_t readIndexHigh = SauConstants::RegisterDepth / 2;
        uint8_t outputCount = 0;
        bool highHalf = false;
        bool readArmed = true;
        SauRegisterFileOutputs output{};
        Beat128 previousBeat{};
    } current, next;

    static RegisterFileState
    nextController(RegisterFileState state,
                   const SauRegisterFileInputs &inputs);
};

} // namespace gem5::sau_mikui

#endif // __SAU_MIKUI_SAU_REGISTER_FILE_HH__
