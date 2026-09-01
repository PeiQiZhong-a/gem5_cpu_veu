#ifndef __SAU_MIKUI_MIKUI_SAU_HH__
#define __SAU_MIKUI_MIKUI_SAU_HH__

#include <deque>
#include <fstream>

#include "base/statistics.hh"
#include "params/MikuiSau.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

#include "brs/sau/sau_endpoint.hh"
#include "sau_mikui/mikui_sau_cycle_model.hh"

namespace gem5::sau_mikui
{

class MikuiSau : public ClockedObject, public brs::SauEndpoint
{
  public:
    using Params = MikuiSauParams;
    explicit MikuiSau(const Params &params);
    ~MikuiSau() override = default;

    void startup() override;
    void reset() override;
    brs::SauResponse evaluate() const override;
    void clock(const brs::SauRequest &request) override;
    brs::SauMemoryOutput evaluateMemory() const override;
    void clockMemory(const brs::SauMemoryResponse &response) override;
    void clockTick(const brs::SauRequest &request,
                   const brs::SauMemoryResponse &response) override;

    uint64_t
    sauCycle() const
    {
        return model.cycle();
    }

  private:
    template <typename T> struct Stamped
    {
        T value{};
        Tick produced = 0;
    };

    struct WrapperStats : public statistics::Group
    {
        explicit WrapperStats(statistics::Group *parent);
        statistics::Scalar acceptedCommands;
        statistics::Scalar completedCommands;
        std::array<statistics::Scalar, 4> commandsByMode;
        std::array<statistics::Scalar, 6> schedulerStateCycles;
        statistics::Scalar activeCycles;
        statistics::Scalar idleCycles;
        statistics::Scalar clockGatedCycles;
        statistics::Scalar readBeats;
        statistics::Scalar writeBeats;
        statistics::Scalar feederActiveCycles;
        statistics::Scalar transposerActiveCycles;
        statistics::Scalar arrayActiveCycles;
        statistics::Scalar outputActiveCycles;
        statistics::Scalar transposerErrors;
        statistics::Scalar timingErrors;
        statistics::Scalar earlyResponseErrors;
        statistics::Scalar missingResponseErrors;
        statistics::Scalar illegalWriteMaskErrors;
        statistics::Scalar illegalConfigurationErrors;
    } wrapperStats;

    MikuiSauCycleModel model;
    EventFunctionWrapper tickEvent;
    bool clockEnabled;
    bool requestHeld = false;
    Tick previousCpuTick = 0;
    Tick observedCpuPeriod = 0;
    Tick lastSauTick = 0;
    bool hasLastSauTick = false;
    brs::SauResponse cpuResponse{};
    std::deque<Stamped<brs::SauRequest>> cpuRequests;
    std::deque<Stamped<brs::SauMemoryResponse>> memoryResponses;
    std::deque<Stamped<brs::SauResponse>> cpuResponses;
    std::deque<Stamped<brs::Sram128Request>> memoryRequests;
    std::deque<Stamped<bool>> crossbarStarts;
    std::deque<Stamped<bool>> crossbarDones;
    std::string tracePath;
    std::ofstream trace;

    void processSauEdge();
    void wakeup();
    bool sameClockLockstep() const;
    bool
    visible(Tick produced) const
    {
        return produced < curTick();
    }
    bool memoryResponseVisible(Tick produced) const;
    void updateStats(const MikuiSauStats &before);
};

} // namespace gem5::sau_mikui

#endif // __SAU_MIKUI_MIKUI_SAU_HH__
