#include "sau_mikui/mikui_sau.hh"

#include "base/logging.hh"
#include "base/output.hh"

namespace gem5::sau_mikui
{

MikuiSau::WrapperStats::WrapperStats(statistics::Group *parent)
    : statistics::Group(parent, "mikuiSau"),
      ADD_STAT(acceptedCommands, statistics::units::Count::get(),
               "Accepted Mikui commands"),
      ADD_STAT(completedCommands, statistics::units::Count::get(),
               "Completed Mikui commands"),
      commandsByMode{{
          {this, "gemmCommands", statistics::units::Count::get(),
           "Accepted GEMM commands"},
          {this, "convCommands", statistics::units::Count::get(),
           "Accepted convolution commands"},
          {this, "transposerCommands", statistics::units::Count::get(),
           "Accepted standalone transposer commands"},
          {this, "addCommands", statistics::units::Count::get(),
           "Accepted matrix-add commands"},
      }},
      schedulerStateCycles{{
          {this, "schedulerIdleCycles", statistics::units::Cycle::get(),
           "Scheduler cycles in IDLE"},
          {this, "schedulerRegisterLoadCycles",
           statistics::units::Cycle::get(),
           "Scheduler cycles in REGISTER_LOAD"},
          {this, "schedulerTransposeLoadCycles",
           statistics::units::Cycle::get(),
           "Scheduler cycles in TRANSPOSE_LOAD"},
          {this, "schedulerFirstLoadCycles", statistics::units::Cycle::get(),
           "Scheduler cycles in FIRST_LOAD"},
          {this, "schedulerReuseLoadCycles", statistics::units::Cycle::get(),
           "Scheduler cycles in REUSE_LOAD"},
          {this, "schedulerDoutCycles", statistics::units::Cycle::get(),
           "Scheduler cycles in D_OUT"},
      }},
      ADD_STAT(activeCycles, statistics::units::Cycle::get(),
               "Active SAU clock cycles"),
      ADD_STAT(idleCycles, statistics::units::Cycle::get(),
               "Idle SAU clock cycles"),
      ADD_STAT(clockGatedCycles, statistics::units::Cycle::get(),
               "Wakeups suppressed while the SAU clock is disabled"),
      ADD_STAT(readBeats, statistics::units::Count::get(), "SRAM read beats"),
      ADD_STAT(writeBeats, statistics::units::Count::get(),
               "SRAM write beats"),
      ADD_STAT(feederActiveCycles, statistics::units::Cycle::get(),
               "Cycles with an active feeder channel"),
      ADD_STAT(transposerActiveCycles, statistics::units::Cycle::get(),
               "Cycles with an active transposer output"),
      ADD_STAT(arrayActiveCycles, statistics::units::Cycle::get(),
               "Cycles with a non-idle array engine"),
      ADD_STAT(outputActiveCycles, statistics::units::Cycle::get(),
               "Cycles with a valid output row"),
      ADD_STAT(transposerErrors, statistics::units::Count::get(),
               "Transposer protocol errors"),
      ADD_STAT(timingErrors, statistics::units::Count::get(),
               "Strict SRAM timing errors"),
      ADD_STAT(earlyResponseErrors, statistics::units::Count::get(),
               "SRAM responses arriving before an outstanding request"),
      ADD_STAT(missingResponseErrors, statistics::units::Count::get(),
               "SRAM responses missing at the configured fixed latency"),
      ADD_STAT(illegalWriteMaskErrors, statistics::units::Count::get(),
               "SRAM writes with an illegal byte mask"),
      ADD_STAT(illegalConfigurationErrors, statistics::units::Count::get(),
               "CSR writes attempted while a command is processing")
{}

MikuiSau::MikuiSau(const Params &params)
    : ClockedObject(params),
      wrapperStats(this),
      model(params.strict_timing),
      tickEvent([this] { processSauEdge(); }, name() + ".sauTick"),
      clockEnabled(params.sau_clk_en),
      tracePath(params.cycle_trace_file)
{
    validateArchitecture(params.rows, params.cols, params.sram_delay_cycles);
    fatal_if(params.trace_internal_pe && tracePath.empty(),
             "trace_internal_pe requires cycle_trace_file");
}

void
MikuiSau::startup()
{
    if (!tracePath.empty()) {
        trace.open(simout.resolve(tracePath), std::ios::out | std::ios::trunc);
        fatal_if(!trace.is_open(), "Cannot open Mikui SAU trace '%s'",
                 tracePath);
        model.writeTraceHeader(trace);
    }
}

void
MikuiSau::reset()
{
    if (tickEvent.scheduled()) {
        deschedule(tickEvent);
    }
    model.reset();
    requestHeld = false;
    previousCpuTick = 0;
    observedCpuPeriod = 0;
    lastSauTick = 0;
    hasLastSauTick = false;
    cpuResponse = {};
    cpuRequests.clear();
    memoryResponses.clear();
    cpuResponses.clear();
    memoryRequests.clear();
    crossbarStarts.clear();
    crossbarDones.clear();
}

brs::SauResponse
MikuiSau::evaluate() const
{
    return cpuResponse;
}

void
MikuiSau::clock(const brs::SauRequest &request)
{
    clockTick(request, {});
}

brs::SauMemoryOutput
MikuiSau::evaluateMemory() const
{
    brs::SauMemoryOutput output;
    if (!memoryRequests.empty() && visible(memoryRequests.front().produced)) {
        output.request = memoryRequests.front().value;
    }
    output.crossbarStart =
        !crossbarStarts.empty() && visible(crossbarStarts.front().produced);
    output.crossbarDone =
        !crossbarDones.empty() && visible(crossbarDones.front().produced);
    return output;
}

void
MikuiSau::clockMemory(const brs::SauMemoryResponse &response)
{
    clockTick({}, response);
}

void
MikuiSau::clockTick(const brs::SauRequest &request,
                    const brs::SauMemoryResponse &response)
{
    if (previousCpuTick != 0 && curTick() > previousCpuTick) {
        observedCpuPeriod = curTick() - previousCpuTick;
    }
    previousCpuTick = curTick();

    // Once equal periods are observed, keep the two synchronous domains in
    // one deterministic CPU-then-SAU phase.  A self-scheduled SAU event has
    // an older event-queue sequence number than the CPU's next event and
    // would otherwise move ahead of the CPU after the first coincident edge.
    if (sameClockLockstep() && tickEvent.scheduled() &&
        tickEvent.when() > curTick()) {
        deschedule(tickEvent);
    }

    cpuResponse = {};
    if (!cpuResponses.empty() && visible(cpuResponses.front().produced)) {
        cpuResponse = cpuResponses.front().value;
        cpuResponses.pop_front();
    }

    if (request.hasTransaction() && !requestHeld) {
        cpuRequests.push_back({request, curTick()});
        requestHeld = true;
    } else if (!request.hasTransaction()) {
        requestHeld = false;
    }
    if (response.valid) {
        memoryResponses.push_back({response, curTick()});
    }

    if (!memoryRequests.empty() && visible(memoryRequests.front().produced)) {
        memoryRequests.pop_front();
    }
    if (!crossbarStarts.empty() && visible(crossbarStarts.front().produced)) {
        crossbarStarts.pop_front();
    }
    if (!crossbarDones.empty() && visible(crossbarDones.front().produced)) {
        crossbarDones.pop_front();
    }
    wakeup();
}

bool
MikuiSau::memoryResponseVisible(Tick produced) const
{
    // In dut_mikui_dma the SAU and crossbar share a clock. gem5 runs the CPU
    // event first at a coincident Tick, so a response sampled from the old
    // crossbar pins is valid input to the later SAU event at that synchronous
    // edge. Asynchronous clocks retain the strict mailbox rule.
    return visible(produced) || (sameClockLockstep() && produced == curTick());
}

bool
MikuiSau::sameClockLockstep() const
{
    return observedCpuPeriod != 0 && observedCpuPeriod == clockPeriod();
}

void
MikuiSau::wakeup()
{
    if (!clockEnabled) {
        ++wrapperStats.clockGatedCycles;
        return;
    }
    if (sameClockLockstep()) {
        if ((!hasLastSauTick || lastSauTick != curTick()) &&
            !tickEvent.scheduled()) {
            schedule(tickEvent, curTick());
        }
        return;
    }
    if (!tickEvent.scheduled()) {
        schedule(tickEvent, clockEdge(Cycles(1)));
    }
}

void
MikuiSau::updateStats(const MikuiSauStats &before)
{
    const auto &after = model.stats();
    wrapperStats.acceptedCommands +=
        after.acceptedCommands - before.acceptedCommands;
    wrapperStats.completedCommands +=
        after.completedCommands - before.completedCommands;
    for (unsigned mode = 0; mode < after.commandByMode.size(); ++mode) {
        wrapperStats.commandsByMode[mode] +=
            after.commandByMode[mode] - before.commandByMode[mode];
    }
    for (unsigned state = 0; state < after.schedulerStateCycles.size();
         ++state) {
        wrapperStats.schedulerStateCycles[state] +=
            after.schedulerStateCycles[state] -
            before.schedulerStateCycles[state];
    }
    wrapperStats.activeCycles += after.activeCycles - before.activeCycles;
    wrapperStats.idleCycles += after.idleCycles - before.idleCycles;
    wrapperStats.readBeats += after.sramReadBeats - before.sramReadBeats;
    wrapperStats.writeBeats += after.sramWriteBeats - before.sramWriteBeats;
    wrapperStats.feederActiveCycles +=
        after.feederActiveCycles - before.feederActiveCycles;
    wrapperStats.transposerActiveCycles +=
        after.transposerActiveCycles - before.transposerActiveCycles;
    wrapperStats.arrayActiveCycles +=
        after.arrayActiveCycles - before.arrayActiveCycles;
    wrapperStats.outputActiveCycles +=
        after.outputActiveCycles - before.outputActiveCycles;
    wrapperStats.transposerErrors +=
        after.transposerErrors - before.transposerErrors;
    wrapperStats.timingErrors += after.timingErrors - before.timingErrors;
    wrapperStats.earlyResponseErrors +=
        after.earlyResponseErrors - before.earlyResponseErrors;
    wrapperStats.missingResponseErrors +=
        after.missingResponseErrors - before.missingResponseErrors;
    wrapperStats.illegalWriteMaskErrors +=
        after.illegalWriteMaskErrors - before.illegalWriteMaskErrors;
    wrapperStats.illegalConfigurationErrors +=
        after.illegalConfigurationErrors - before.illegalConfigurationErrors;
}

void
MikuiSau::processSauEdge()
{
    lastSauTick = curTick();
    hasLastSauTick = true;

    brs::SauRequest request;
    brs::SauMemoryResponse response;
    if (!cpuRequests.empty() && visible(cpuRequests.front().produced)) {
        request = cpuRequests.front().value;
        cpuRequests.pop_front();
    }
    if (!memoryResponses.empty() &&
        memoryResponseVisible(memoryResponses.front().produced)) {
        response = memoryResponses.front().value;
        memoryResponses.pop_front();
    }

    const auto before = model.stats();
    const auto oldMemory = model.evaluateMemory();
    model.clockEdge(request, response);
    const auto newResponse = model.evaluate();
    const auto newMemory = model.evaluateMemory();
    if (newResponse.valid) {
        cpuResponses.push_back({newResponse, curTick()});
    }
    if (newMemory.request.valid) {
        memoryRequests.push_back({newMemory.request, curTick()});
    }
    if (newMemory.crossbarStart && !oldMemory.crossbarStart) {
        crossbarStarts.push_back({true, curTick()});
    }
    if (newMemory.crossbarDone && !oldMemory.crossbarDone) {
        crossbarDones.push_back({true, curTick()});
    }
    updateStats(before);
    if (trace.is_open()) {
        model.writeTrace(trace, curTick());
        trace.flush();
    }

    if (!sameClockLockstep() &&
        (model.active() || !cpuRequests.empty() || !memoryResponses.empty())) {
        schedule(tickEvent, clockEdge(Cycles(1)));
    }
}

} // namespace gem5::sau_mikui
