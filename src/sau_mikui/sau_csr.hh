#ifndef __SAU_MIKUI_SAU_CSR_HH__
#define __SAU_MIKUI_SAU_CSR_HH__

#include <cstdint>

#include "sau_mikui/sau_types.hh"

namespace gem5::sau_mikui
{

struct SauCsrRequest
{
    bool write = false;
    bool read = false;
    uint8_t writeType = 0;
    uint16_t address = 0;
    uint64_t writeData = 0;

    constexpr bool
    hasTransaction() const
    {
        return write || read;
    }
};

struct SauCsrInputs
{
    SauCsrRequest request;
    bool flowEnd = false;
};

struct SauCsrOutputs
{
    bool ready = false;
    uint32_t readData = 0;
    bool start = false;
    bool busy = false;
    bool crossbarStart = false;
    bool crossbarError = false;
    SauCommand command;
};

// Direct translation of hardware/src/sa_element/csr.sv. State advances only
// in commit(), so other modules cannot observe same-edge writes.
class SauCsr
{
  public:
    void reset();
    SauCsrOutputs evaluate() const;
    void computeNext(const SauCsrInputs &inputs);
    void commit();

  private:
    struct State
    {
        SauCommand command;
        bool start = false;
        bool busy = false;
        bool processing = false;
        bool ready = false;
        bool error = false;
        uint32_t readData = 0;
    } current, next;

    static bool mapped(uint16_t address);
    static uint8_t slot(uint16_t address);
    static bool highHalf(uint16_t address);
    static uint32_t readSlot(const State &state, uint8_t slot, bool high);
    static void writeSlot(State &state, uint8_t slot, uint8_t writeType,
                          uint64_t value);
};

} // namespace gem5::sau_mikui

#endif // __SAU_MIKUI_SAU_CSR_HH__
