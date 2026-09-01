#ifndef __SAU_MIKUI_MIKUI_SAU_CYCLE_MODEL_HH__
#define __SAU_MIKUI_MIKUI_SAU_CYCLE_MODEL_HH__

#include <cstdint>
#include <iosfwd>

#include "brs/sau/sau_endpoint.hh"
#include "sau_mikui/sau_address_generator.hh"
#include "sau_mikui/sau_array_engine.hh"
#include "sau_mikui/sau_csr.hh"
#include "sau_mikui/sau_feeder.hh"
#include "sau_mikui/sau_memory_controller.hh"
#include "sau_mikui/sau_output_path.hh"
#include "sau_mikui/sau_register_file.hh"
#include "sau_mikui/sau_scheduler.hh"
#include "sau_mikui/sau_shift_register.hh"
#include "sau_mikui/sau_transposer.hh"

namespace gem5::sau_mikui
{

struct MikuiSauStats
{
    uint64_t cycles = 0;
    uint64_t activeCycles = 0;
    uint64_t idleCycles = 0;
    uint64_t acceptedCommands = 0;
    uint64_t completedCommands = 0;
    std::array<uint64_t, 4> commandByMode{};
    std::array<uint64_t, 6> schedulerStateCycles{};
    uint64_t sramReadBeats = 0;
    uint64_t sramWriteBeats = 0;
    uint64_t feederActiveCycles = 0;
    uint64_t transposerActiveCycles = 0;
    uint64_t arrayActiveCycles = 0;
    uint64_t outputActiveCycles = 0;
    uint64_t transposerErrors = 0;
    uint64_t timingErrors = 0;
    uint64_t earlyResponseErrors = 0;
    uint64_t missingResponseErrors = 0;
    uint64_t illegalWriteMaskErrors = 0;
    uint64_t illegalConfigurationErrors = 0;
};

struct MikuiSauCommandMilestones
{
    uint64_t sequence = 0;
    uint64_t start = 0;
    uint64_t firstRead = 0;
    uint64_t firstArrayInput = 0;
    uint64_t firstResult = 0;
    uint64_t lastWrite = 0;
    uint64_t done = 0;
    bool active = false;
    bool sawFirstRead = false;
    bool sawFirstArrayInput = false;
    bool sawFirstResult = false;
    bool sawLastWrite = false;
};

class MikuiSauCycleModel
{
  public:
    explicit MikuiSauCycleModel(bool strictTiming = true);

    void reset();
    brs::SauResponse evaluate() const;
    brs::SauMemoryOutput evaluateMemory() const;
    void clockEdge(const brs::SauRequest &request,
                   const brs::SauMemoryResponse &memoryResponse);

    bool active() const;
    uint64_t
    cycle() const
    {
        return statistics.cycles;
    }
    const MikuiSauStats &
    stats() const
    {
        return statistics;
    }
    const SauMemoryControllerErrors &
    memoryErrors() const
    {
        return memory.errors();
    }
    void writeTraceHeader(std::ostream &stream) const;
    void writeTrace(std::ostream &stream, uint64_t tick = 0) const;

  private:
    bool strict;
    SauCsr csr;
    SauScheduler scheduler;
    SauAddressGenerator address;
    SauMemoryController memory;
    SauRegisterFile registerFile;
    SauShiftRegister shiftRegister;
    SauFeeder feeder;
    std::array<SauTransposer, 3> transposer;
    SauArrayEngine array;
    SauOutputPath output;
    MikuiSauStats statistics{};
    brs::SauRequest lastRequest{};
    bool lastMemoryResponseValid = false;
    MikuiSauCommandMilestones commandMilestones{};
    uint32_t traceEvents = 0;

    Row16 transposedResult() const;
};

} // namespace gem5::sau_mikui

#endif // __SAU_MIKUI_MIKUI_SAU_CYCLE_MODEL_HH__
