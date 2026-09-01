#include "sau_mikui/mikui_sau_cycle_model.hh"

#include <ostream>

namespace gem5::sau_mikui
{

MikuiSauCycleModel::MikuiSauCycleModel(bool strictTiming)
    : strict(strictTiming), memory(strictTiming)
{
    validateArchitecture(SauConstants::Rows, SauConstants::Cols,
                         SauConstants::SramDelay);
    reset();
}

void
MikuiSauCycleModel::reset()
{
    csr.reset();
    scheduler.reset();
    address.reset();
    memory.reset();
    registerFile.reset();
    shiftRegister.reset();
    feeder.reset();
    for (auto &unit : transposer) {
        unit.reset();
    }
    array.reset();
    output.reset();
    statistics = {};
    lastRequest = {};
    lastMemoryResponseValid = false;
    commandMilestones = {};
    traceEvents = 0;
}

brs::SauResponse
MikuiSauCycleModel::evaluate() const
{
    const auto value = csr.evaluate();
    return {value.ready, value.readData};
}

brs::SauMemoryOutput
MikuiSauCycleModel::evaluateMemory() const
{
    const auto csrOutput = csr.evaluate();
    const auto schedulerOutput = scheduler.evaluate({});
    return {
        memory.evaluate().request,
        csrOutput.crossbarStart,
        schedulerOutput.crossbarDone,
    };
}

Row16
MikuiSauCycleModel::transposedResult() const
{
    Row16 result{};
    const auto low = transposer[2].evaluate();
    const auto high = transposer[1].evaluate();
    for (unsigned col = 0; col < SauConstants::Cols; ++col) {
        const uint8_t lowByte = static_cast<uint8_t>(low.row[col]);
        const uint8_t highByte = static_cast<uint8_t>(high.row[col]);
        result[col] =
            static_cast<int16_t>(static_cast<uint16_t>(lowByte) |
                                 (static_cast<uint16_t>(highByte) << 8));
    }
    return result;
}

void
MikuiSauCycleModel::clockEdge(const brs::SauRequest &request,
                              const brs::SauMemoryResponse &memoryResponse)
{
    enum TraceEvent : uint32_t
    {
        CommandStart = 1U << 0,
        FirstRead = 1U << 1,
        FirstArrayInput = 1U << 2,
        FirstResult = 1U << 3,
        LastWrite = 1U << 4,
        CommandDone = 1U << 5,
    };
    traceEvents = 0;
    lastRequest = request;
    lastMemoryResponseValid = memoryResponse.valid;
    const auto csrOut = csr.evaluate();
    // Transposer storage is instruction-local.  The PE accumulators live in
    // SauArrayEngine and intentionally survive RETAIN/TRETAIN boundaries.
    if (csrOut.start) {
        for (auto &unit : transposer) {
            unit.reset();
        }
    }
    const auto addressOut = address.evaluate();
    const auto memoryOut = memory.evaluate();
    const auto registerOut = registerFile.evaluate();
    const auto shiftOut = shiftRegister.evaluate();
    const auto feederOut = feeder.evaluate();
    const auto trans0 = transposer[0].evaluate();
    const auto trans1 = transposer[1].evaluate();
    const auto trans2 = transposer[2].evaluate();
    const auto arrayOut = array.evaluate();
    const auto outputOut = output.evaluate();

    SauSchedulerInputs schedulerInputs;
    schedulerInputs.start = csrOut.start;
    schedulerInputs.convKernel = csrOut.command.convKernel;
    schedulerInputs.reuseMode = csrOut.command.reuseMode;
    schedulerInputs.transposeMode = csrOut.command.transposeMode;
    schedulerInputs.flowTimes = csrOut.command.flowLoopTimes;
    schedulerInputs.shift = csrOut.command.shift;
    schedulerInputs.lastInstruction = csrOut.command.lastInstruction;
    schedulerInputs.loadDone = addressOut.loadDone;
    schedulerInputs.executeFinished = arrayOut.peFinish;
    schedulerInputs.updateFinished = feederOut.updateFinished;
    schedulerInputs.writeFinished = memoryOut.lastInstructionWriteDone;
    const bool keepMode = csrOut.command.flowMode == FlowMode::Retain ||
                          csrOut.command.flowMode == FlowMode::Tretain;
    schedulerInputs.registerFileClear =
        addressOut.registerFileClear || (keepMode && feederOut.updateFinished);
    schedulerInputs.lastInstructionWriteDone =
        memoryOut.lastInstructionWriteDone;
    schedulerInputs.lastFlowTimeClear = feederOut.lastFlowTimeClear;
    const auto schedulerOut = scheduler.evaluate(schedulerInputs);

    SauCsrInputs csrInputs;
    csrInputs.request = {
        request.csrWrite, request.csrRead,   request.writeType,
        request.csrAddr,  request.writeData,
    };
    csrInputs.flowEnd = schedulerOut.flowEnd;
    csr.computeNext(csrInputs);

    scheduler.computeNext(schedulerInputs);

    SauAddressInputs addressInputs;
    addressInputs.start = csrOut.start;
    addressInputs.state = schedulerOut.state;
    addressInputs.inputSwitch = schedulerOut.inputSwitch;
    addressInputs.lastFlowTime = schedulerOut.lastFlowTime;
    addressInputs.writeValid = feederOut.writeAddressValid;
    addressInputs.writeLast = feederOut.writeAddressLast;
    addressInputs.command = csrOut.command;
    address.computeNext(addressInputs);

    SauMemoryControllerInputs memoryInputs;
    memoryInputs.address = addressOut.address;
    memoryInputs.readEnable = addressOut.readEnable;
    memoryInputs.writeEnable = addressOut.writeEnable;
    memoryInputs.readLast = addressOut.readLast;
    memoryInputs.writeData = feederOut.writeData;
    memoryInputs.writeLast = feederOut.writeDataLast;
    memoryInputs.lastInstruction = csrOut.command.lastInstruction;
    memoryInputs.schedulerState = schedulerOut.state;
    memoryInputs.response = memoryResponse;
    memory.computeNext(memoryInputs);

    SauRegisterFileInputs registerInputs;
    registerInputs.writeValid = feederOut.registerWriteValid;
    registerInputs.writeData = feederOut.registerWriteData;
    registerInputs.readEnable = feederOut.registerReadEnable;
    registerInputs.clear = addressOut.registerFileClear;
    registerInputs.kernel = csrOut.command.convKernel;
    registerInputs.registerMode = csrOut.command.registerMode;
    registerInputs.stride = csrOut.command.stride;
    registerInputs.shift = csrOut.command.shift;
    registerFile.computeNext(registerInputs);

    SauShiftRegisterInputs shiftInputs;
    shiftInputs.valid = registerOut.valid;
    shiftInputs.data = registerOut.data;
    shiftInputs.last = registerOut.last;
    shiftInputs.kernel = csrOut.command.convKernel;
    shiftInputs.stride = csrOut.command.stride;
    shiftRegister.computeNext(shiftInputs);

    const bool outputTranspose =
        csrOut.command.transposeMode == TransposeMode::Abdt ||
        csrOut.command.flowMode == FlowMode::Ctrans;
    const bool convolution = csrOut.command.convKernel >= 3;
    const bool outputTransposePhase =
        outputTranspose && (arrayOut.state == ArrayEngineState::Storage ||
                            arrayOut.state == ArrayEngineState::Done ||
                            outputOut.valid || trans2.outputReady);
    const bool outputTransposeValid =
        trans2.valid && (!csrOut.command.shift || trans1.valid);
    SauFeederInputs feederInputs;
    feederInputs.start = csrOut.start;
    feederInputs.command = csrOut.command;
    feederInputs.schedulerState = schedulerOut.state;
    feederInputs.inputSwitch = schedulerOut.inputSwitch;
    feederInputs.lastFlowTime = schedulerOut.lastFlowTime;
    feederInputs.memoryValid = memoryOut.readValid;
    feederInputs.memoryData = memoryOut.readData;
    feederInputs.memoryLast = memoryOut.readLast;
    feederInputs.registerValid = shiftOut.valid;
    feederInputs.registerData = shiftOut.data;
    feederInputs.registerLast = shiftOut.last;
    feederInputs.resultValid =
        outputTranspose ? outputTransposeValid : outputOut.valid;
    feederInputs.resultIndexValid = !outputTranspose;
    feederInputs.resultRowIndex = outputOut.rowIndex;
    feederInputs.resultData =
        outputTranspose ? transposedResult() : outputOut.row;
    feederInputs.resultLast = outputTranspose ? trans2.last : outputOut.last;
    feederInputs.executeDone = arrayOut.peFinish;
    feeder.computeNext(feederInputs);

    const bool inputReady = trans0.outputReady && trans1.outputReady;
    // SA_ENGINE accepts EN_i in STORAGE. This is required for a command after
    // RETAIN, whose PE accumulators deliberately remain live in that state.
    const bool readInputs = inputReady;
    SauTransposerInputs transInputs[3];
    transInputs[0].writeEnable = !convolution && feederOut.aValid;
    transInputs[0].row = feederOut.a;
    transInputs[0].readEnable = readInputs;
    transInputs[0].transpose =
        csrOut.command.transposeMode == TransposeMode::Atbd;

    Row8 outputLow{};
    Row8 outputHigh{};
    for (unsigned col = 0; col < SauConstants::Cols; ++col) {
        outputLow[col] = static_cast<int8_t>(outputOut.row[col] & 0xff);
        outputHigh[col] = static_cast<int8_t>(
            (static_cast<uint16_t>(outputOut.row[col]) >> 8) & 0xff);
    }
    transInputs[1].writeEnable = outputTransposePhase && csrOut.command.shift
                                     ? outputOut.valid
                                     : feederOut.bValid;
    transInputs[1].row = outputTransposePhase && csrOut.command.shift
                             ? outputHigh
                             : feederOut.b;
    if (convolution && !outputTransposePhase) {
        transInputs[1].writeEnable = false;
    }
    transInputs[1].readEnable = outputTransposePhase
                                    ? trans1.outputReady && trans2.outputReady
                                    : readInputs;
    transInputs[1].transpose =
        outputTransposePhase ||
        csrOut.command.transposeMode == TransposeMode::Abtd;

    transInputs[2].writeEnable = outputTranspose && outputOut.valid;
    transInputs[2].row = outputLow;
    transInputs[2].readEnable = outputTranspose && trans2.outputReady &&
                                (!csrOut.command.shift || trans1.outputReady);
    transInputs[2].transpose = true;
    for (unsigned i = 0; i < 3; ++i) {
        transposer[i].computeNext(transInputs[i]);
    }

    SauArrayEngineInputs arrayInputs;
    arrayInputs.start = csrOut.start;
    arrayInputs.command = csrOut.command;
    arrayInputs.aValid = convolution ? feederOut.aValid : trans0.valid;
    arrayInputs.a = convolution ? feederOut.a : trans0.row;
    arrayInputs.aLast = convolution ? feederOut.aLast : trans0.last;
    arrayInputs.bValid = convolution ? feederOut.bValid
                                     : (trans1.valid && !outputTransposePhase);
    arrayInputs.b = convolution ? feederOut.b : trans1.row;
    arrayInputs.bLast = convolution ? feederOut.bLast : trans1.last;
    arrayInputs.cValid = feederOut.cValid;
    arrayInputs.c = feederOut.c;
    array.computeNext(arrayInputs);

    const uint64_t edgeCycle = statistics.cycles;
    if (csrOut.start) {
        commandMilestones = {};
        commandMilestones.active = true;
        commandMilestones.sequence = statistics.acceptedCommands + 1;
        commandMilestones.start = edgeCycle;
        traceEvents |= CommandStart;
    }
    if (commandMilestones.active && memoryOut.request.valid &&
        !memoryOut.request.isWrite() && !commandMilestones.sawFirstRead) {
        commandMilestones.sawFirstRead = true;
        commandMilestones.firstRead = edgeCycle;
        traceEvents |= FirstRead;
    }
    if (commandMilestones.active &&
        (arrayInputs.aValid || arrayInputs.bValid || arrayInputs.cValid) &&
        !commandMilestones.sawFirstArrayInput) {
        commandMilestones.sawFirstArrayInput = true;
        commandMilestones.firstArrayInput = edgeCycle;
        traceEvents |= FirstArrayInput;
    }
    if (commandMilestones.active && outputOut.valid &&
        !commandMilestones.sawFirstResult) {
        commandMilestones.sawFirstResult = true;
        commandMilestones.firstResult = edgeCycle;
        traceEvents |= FirstResult;
    }
    if (commandMilestones.active && memoryOut.request.valid &&
        memoryOut.request.isWrite() && feederOut.writeDataLast) {
        commandMilestones.sawLastWrite = true;
        commandMilestones.lastWrite = edgeCycle;
        traceEvents |= LastWrite;
    }
    if (commandMilestones.active && schedulerOut.crossbarDone) {
        commandMilestones.done = edgeCycle;
        commandMilestones.active = false;
        traceEvents |= CommandDone;
    }

    SauOutputPathInputs outputInputs;
    outputInputs.valid = arrayOut.rowValid;
    outputInputs.rowIndex = arrayOut.rowIndex;
    outputInputs.row = arrayOut.row;
    outputInputs.last = arrayOut.calculateFinish;
    outputInputs.cutbit = csrOut.command.cutbit;
    outputInputs.shift = csrOut.command.shift;
    output.computeNext(outputInputs);

    csr.commit();
    scheduler.commit();
    address.commit();
    memory.commit();
    registerFile.commit();
    shiftRegister.commit();
    feeder.commit();
    for (auto &unit : transposer) {
        unit.commit();
    }
    array.commit();
    output.commit();

    ++statistics.cycles;
    if (active()) {
        ++statistics.activeCycles;
    } else {
        ++statistics.idleCycles;
    }
    if (csrOut.start) {
        ++statistics.acceptedCommands;
        ++statistics
              .commandByMode[static_cast<unsigned>(csrOut.command.operation)];
    }
    if (schedulerOut.crossbarDone) {
        ++statistics.completedCommands;
    }
    ++statistics
          .schedulerStateCycles[static_cast<unsigned>(schedulerOut.state)];
    if (memoryOut.request.valid) {
        if (memoryOut.request.isWrite()) {
            ++statistics.sramWriteBeats;
        } else {
            ++statistics.sramReadBeats;
        }
    }
    if (feederOut.aValid || feederOut.bValid || feederOut.cValid) {
        ++statistics.feederActiveCycles;
    }
    if (trans0.valid || trans1.valid || trans2.valid) {
        ++statistics.transposerActiveCycles;
    }
    if (arrayOut.state != ArrayEngineState::Idle) {
        ++statistics.arrayActiveCycles;
    }
    if (outputOut.valid) {
        ++statistics.outputActiveCycles;
    }
    statistics.transposerErrors += trans0.error + trans1.error + trans2.error;
    statistics.earlyResponseErrors = memory.errors().earlyResponses;
    statistics.missingResponseErrors = memory.errors().missingResponses;
    statistics.illegalWriteMaskErrors = memory.errors().illegalWriteMasks;
    statistics.timingErrors = statistics.earlyResponseErrors +
                              statistics.missingResponseErrors +
                              statistics.illegalWriteMaskErrors;
    statistics.illegalConfigurationErrors += csrOut.crossbarError;
    (void)strict;
}

bool
MikuiSauCycleModel::active() const
{
    return csr.evaluate().busy ||
           scheduler.evaluate({}).state != SchedulerState::Idle ||
           memory.evaluate().request.valid ||
           feeder.evaluate().writeDataValid ||
           array.evaluate().state != ArrayEngineState::Idle;
}

void
MikuiSauCycleModel::writeTraceHeader(std::ostream &stream) const
{
    stream << "tick,sau_cycle,event,command_seq,csr_we,csr_re,csr_addr,"
              "csr_ready,start,busy,"
              "scheduler_state,"
              "input_switch,flow_count,last_flow,flow_end,sram_req,"
              "sram_addr,sram_wstrb,sram_rvalid,feeder_a_valid,"
              "feeder_a0,feeder_b_valid,feeder_b0,feeder_c_valid,"
              "feeder_c0,trans0_ready,trans0_valid,trans0_0,"
              "trans1_ready,trans1_valid,trans1_0,trans2_ready,trans2_valid,"
              "array_state,pe_finish,storage_ready,row_valid,row_index,"
              "calculate_finish,result_valid,result_last,"
              "xbar_start,xbar_done,response_valid,timing_errors\n";
}

void
MikuiSauCycleModel::writeTrace(std::ostream &stream, uint64_t tick) const
{
    const auto c = csr.evaluate();
    const auto s = scheduler.evaluate({});
    const auto m = memory.evaluate();
    const auto f = feeder.evaluate();
    const auto a = array.evaluate();
    const auto o = output.evaluate();
    const auto t0 = transposer[0].evaluate();
    const auto t1 = transposer[1].evaluate();
    const auto t2 = transposer[2].evaluate();
    std::string event;
    const std::array<std::pair<uint32_t, const char *>, 6> eventNames{{
        {1U << 0, "start"}, {1U << 1, "first_read"},
        {1U << 2, "first_array_input"}, {1U << 3, "first_result"},
        {1U << 4, "last_write"}, {1U << 5, "done"},
    }};
    for (const auto &[mask, name] : eventNames) {
        if (traceEvents & mask) {
            if (!event.empty()) {
                event += '|';
            }
            event += name;
        }
    }
    stream << tick << ',' << statistics.cycles << ',' << event << ','
           << commandMilestones.sequence << ',' << lastRequest.csrWrite
           << ',' << lastRequest.csrRead << ',' << lastRequest.csrAddr << ','
           << c.ready << ',' << c.start << ',' << c.busy << ','
           << static_cast<unsigned>(s.state) << ','
           << static_cast<unsigned>(s.inputSwitch) << ','
           << static_cast<unsigned>(s.flowCount) << ',' << s.lastFlowTime
           << ',' << s.flowEnd << ',' << m.request.valid << ','
           << m.request.address << ',' << m.request.writeStrobe << ','
           << m.readValid << ',' << f.aValid << ',' << static_cast<int>(f.a[0])
           << ',' << f.bValid << ',' << static_cast<int>(f.b[0]) << ','
           << f.cValid << ',' << f.c[0] << ',' << t0.outputReady << ','
           << t0.valid << ',' << static_cast<int>(t0.row[0]) << ','
           << t1.outputReady << ',' << t1.valid << ','
           << static_cast<int>(t1.row[0]) << ',' << t2.outputReady << ','
           << t2.valid << ',' << static_cast<unsigned>(a.state) << ','
           << a.peFinish << ',' << a.storageReady << ',' << a.rowValid << ','
           << static_cast<unsigned>(a.rowIndex) << ',' << a.calculateFinish
           << ',' << o.valid << ',' << o.last << ',' << c.crossbarStart << ','
           << s.crossbarDone << ',' << lastMemoryResponseValid << ','
           << statistics.timingErrors << '\n';
}

} // namespace gem5::sau_mikui
