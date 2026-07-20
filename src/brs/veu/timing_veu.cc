#include "brs/veu/timing_veu.hh"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace gem5
{
namespace brs
{

TimingVeu::TimingVeu()
{
    reset();
}

void
TimingVeu::configure(const VeuTimingConfig &config)
{
    timing = config;
    if (timing.inputFifoDepth == 0 || timing.executeLatency == 0 ||
        timing.executeII == 0 || timing.vsuLatency == 0 ||
        timing.maxOutstandingReads == 0) {
        throw std::runtime_error("TimingVEU cycle and depth parameters must be positive");
    }
    timingProfile.load(timing.timingProfilePath);
    if (timingProfile.loaded() && timing.inputFifoDepth != 4) {
        throw std::runtime_error("calibrated TimingVEU profile requires FIFO depth 4");
    }
    traceStream.close();
    if (!timing.cycleTracePath.empty()) {
        traceStream.open(timing.cycleTracePath, std::ios::out | std::ios::trunc);
        if (!traceStream) {
            throw std::runtime_error("cannot open VEU cycle trace: " +
                                     timing.cycleTracePath);
        }
        traceStream << "cycle,event,pc,instr,op,requested_vlen,effective_vlen,"
                    << "chunk,source,transaction_id,addr,data,wstrb,"
                    << "fifo1,fifo2,fifo3,outstanding,status,lock,detail\n";
    }
}

void
TimingVeu::setMemoryRequestCallback(MemoryRequestFn callback)
{
    memoryRequest = std::move(callback);
}

void
TimingVeu::reset()
{
    currentState = State::Idle;
    controlState = ControlState::Idle;
    csr = {};
    csr.mask = FullWriteMask;
    requestReg = {};
    responseData = 0;
    pendingThreeSourceStart = false;
    pendingThreeSourceVeStart = 0;
    statusBusy = false;
    lockActive = false;
    internalActive = false;
    modelCycle = 0;
    operationId = 0;
    nextOperationId = 1;
    nextTransactionId = 1;
    operationStartCycle = 0;
    nextVfuAcceptCycle = 0;
    drainReadyCycle = 0;
    operationInstruction = VeuInstruction::Unknown;
    operationInfo = {};
    operationChunkCount = 0;
    nextExecuteChunk = 0;
    completedChunkCount = 0;
    readRoundRobin = 0;
    nextReadChunk = {};
    outstandingBySource = {};
    for (auto &fifo : inputFifos) fifo.clear();
    vfuPipeline.clear();
    vsuPipeline.clear();
    storeQueue.clear();
    outstanding.clear();
    pendingResponses.clear();
    retryRequest.reset();
    functionalExecutor.reset();
    acceptedRequests = responses = startedOperations = completedOperations = 0;
    processedChunks = busyCycles = loadWaitCycles = executeCycles = 0;
    storeWaitCycles = memoryReads = memoryWrites = 0;
    statusActiveCycles = lockActiveCycles = maxOutstandingReadsSeen = 0;
    maxFifoOccupancy = {};
    fifoFullStalls = fifoEmptyStalls = 0;
    vfuAccepted = vfuCompleted = maxVfuInFlight = vfuIIStalls = 0;
    vsuQueueStalls = storePriorityCycles = readsBlockedByStore = 0;
    maskedWrites = zeroMaskSkippedWrites = retries = unexpectedResponses = 0;
    profileHits = profileMisses = profileFallbacks = 0;
    zeroLengthNoops = illegalOperations = 0;
}

VeuResponse
TimingVeu::evaluate() const
{
    return {controlState == ControlState::Respond,
            controlState == ControlState::Respond ? responseData : 0};
}

uint32_t
TimingVeu::readCsr(uint16_t addr) const
{
    switch (static_cast<VeuCsr>(addr)) {
      case VeuCsr::Status: return csr.status;
      case VeuCsr::ReadAddress1: return csr.raddr1;
      case VeuCsr::ReadAddress2: return csr.raddr2;
      case VeuCsr::WriteAddress: return csr.waddr;
      case VeuCsr::Config: return csr.config;
      case VeuCsr::VectorLength: return csr.vlen;
      case VeuCsr::Mask: return csr.mask;
      case VeuCsr::ReadAddress3: return csr.raddr3;
      default: return 0;
    }
}

void
TimingVeu::writeCsr(uint16_t addr, uint32_t value, VeuWriteType writeType)
{
    uint32_t *target = nullptr;
    switch (static_cast<VeuCsr>(addr)) {
      case VeuCsr::Status: target = &csr.status; break;
      case VeuCsr::ReadAddress1: target = &csr.raddr1; break;
      case VeuCsr::ReadAddress2: target = &csr.raddr2; break;
      case VeuCsr::WriteAddress: target = &csr.waddr; break;
      case VeuCsr::Config: target = &csr.config; break;
      case VeuCsr::VectorLength: target = &csr.vlen; break;
      case VeuCsr::Mask: target = &csr.mask; break;
      case VeuCsr::ReadAddress3: target = &csr.raddr3; break;
      default: break;
    }
    if (!target) return;
    switch (writeType) {
      case VeuWriteType::Write:
      case VeuWriteType::VectorStart: *target = value; break;
      case VeuWriteType::Set: *target |= value; break;
      case VeuWriteType::Clear: *target &= value; break;
    }
}

VeuInstruction
TimingVeu::decodeStart(uint32_t start) const
{
    for (uint8_t bit = 0; bit < 32; ++bit) {
        if (!(start & (uint32_t{1} << bit))) continue;
        switch (bit) {
          case 0: return VeuInstruction::Add;
          case 1: return VeuInstruction::Sub;
          case 2: return VeuInstruction::Min;
          case 3: return VeuInstruction::Max;
          case 4: return VeuInstruction::ReduceMin;
          case 5: return VeuInstruction::ReduceMax;
          case 6: return VeuInstruction::And;
          case 7: return VeuInstruction::Or;
          case 8: return VeuInstruction::Xor;
          case 11: return VeuInstruction::Move;
          case 12: return VeuInstruction::ShiftRightLogical;
          case 13: return VeuInstruction::ShiftRightArithmetic;
          case 14: return VeuInstruction::NarrowClip;
          case 16: return VeuInstruction::ReduceSum;
          case 18: return VeuInstruction::MultiplySubtract;
          case 19: return VeuInstruction::MultiplyAdd;
          case 20: return VeuInstruction::Multiply;
          case 21: return VeuInstruction::MultiplyHighSignedUnsigned;
          case 22: return VeuInstruction::MultiplyHigh;
          default: return VeuInstruction::Unknown;
        }
    }
    return VeuInstruction::Unknown;
}

void
TimingVeu::acceptRequest(const VeuRequest &request)
{
    requestReg = request;
    ++acceptedRequests;
    const auto writeType = isVeuWriteType(request.writeType) ?
        static_cast<VeuWriteType>(request.writeType) : VeuWriteType::Write;
    responseData = request.csrRead ? readCsr(request.csrAddr) : 0;
    if (request.csrWrite && writeType == VeuWriteType::VectorStart) {
        startVectorOperation(request);
    } else if (request.csrWrite) {
        writeCsr(request.csrAddr, unpackVeuOperand1(request.writeData),
                 writeType);
    }
    controlState = ControlState::Respond;
}

std::string
TimingVeu::maskClassName() const
{
    return csr.mask == 0 ? "zero" :
           (csr.mask == FullWriteMask ? "full" : "partial");
}

std::string
TimingVeu::sourceSetName() const
{
    std::string result;
    for (uint8_t source = 1; source <= 3; ++source) {
        if (!VeuFunctionalExecutor::sourceRequired(
                operationInfo.sourceMask, static_cast<VeuSource>(source))) {
            continue;
        }
        if (!result.empty()) result += "+";
        result += "src" + std::to_string(source);
    }
    return result;
}

void
TimingVeu::startVectorOperation(const VeuRequest &request)
{
    if (operationBusy()) {
        responseData = csr.status;
        return;
    }
    const bool threeSource =
        (request.veStart & veuStartMask(VeuInstruction::MultiplyAdd)) ||
        (request.veStart & veuStartMask(VeuInstruction::MultiplySubtract));
    if (threeSource && !pendingThreeSourceStart) {
        csr.raddr1 = unpackVeuOperand1(request.writeData);
        csr.raddr2 = unpackVeuOperand2(request.writeData);
        pendingThreeSourceStart = true;
        pendingThreeSourceVeStart = request.veStart;
        responseData = csr.status;
        return;
    }
    if (request.csrAddr == static_cast<uint16_t>(VeuCsr::ReadAddress1)) {
        if (pendingThreeSourceStart) {
            csr.raddr3 = unpackVeuOperand1(request.writeData);
        } else {
            csr.raddr1 = unpackVeuOperand1(request.writeData);
            csr.raddr2 = unpackVeuOperand2(request.writeData);
        }
    } else if (request.csrAddr == static_cast<uint16_t>(VeuCsr::ReadAddress3)) {
        csr.raddr3 = unpackVeuOperand1(request.writeData);
    }
    const uint32_t start = pendingThreeSourceStart ?
        pendingThreeSourceVeStart : request.veStart;
    pendingThreeSourceStart = false;
    pendingThreeSourceVeStart = 0;

    if (csr.vlen == 0) {
        ++zeroLengthNoops;
        trace("zero_length_noop");
        responseData = csr.status;
        return;
    }

    operationInstruction = decodeStart(start);
    const bool scalarEnabled = (csr.config & 0x800u) != 0;
    operationInfo = VeuFunctionalExecutor::describe(operationInstruction,
                                                     scalarEnabled);
    if (!operationInfo.supported) {
        ++illegalOperations;
        trace("illegal_operation", -1, VeuSource::None, 0, 0, nullptr, 0,
              "unsupported start bit");
        responseData = csr.status;
        return;
    }

    const uint32_t effectiveBits = effectiveVeuLengthAtStart(csr.vlen);
    operationChunkCount = effectiveBits / VeuVectorBits;
    activeTiming = timingProfile.select(
        operationInfo.name, (csr.config >> 7) & 0x3, scalarEnabled,
        maskClassName(), sourceSetName(), timing.executeLatency,
        timing.executeII, timing.inputFifoDepth, timing.maxOutstandingReads,
        timing.vsuLatency);
    if (activeTiming.matched) {
        ++profileHits;
    } else {
        ++profileMisses;
        ++profileFallbacks;
        if (timingProfile.loaded()) {
            std::cerr << "warning: TimingVEU profile fallback for "
                      << operationInfo.name << '\n';
        }
        trace("timing_profile_fallback");
    }

    operationId = nextOperationId++;
    operationStartCycle = modelCycle;
    nextVfuAcceptCycle = modelCycle + timing.startupCycles;
    nextExecuteChunk = 0;
    completedChunkCount = 0;
    nextReadChunk = {};
    outstandingBySource = {};
    readRoundRobin = 0;
    for (auto &fifo : inputFifos) fifo.clear();
    vfuPipeline.clear();
    vsuPipeline.clear();
    storeQueue.clear();
    outstanding.clear();
    pendingResponses.clear();
    retryRequest.reset();
    functionalExecutor.reset();
    statusBusy = true;
    lockActive = true;
    internalActive = true;
    currentState = State::Running;
    csr.status = (start << 1) | 1u;
    responseData = csr.status;
    ++startedOperations;
    trace("operation_start");
    trace("status_set");
    trace("lock_start");
}

uint64_t
TimingVeu::currentOutstandingReadCount() const
{
    return outstandingBySource[0] + outstandingBySource[1] +
           outstandingBySource[2];
}

uint64_t
TimingVeu::fifoMaxOccupancy(unsigned source) const
{
    return source < maxFifoOccupancy.size() ? maxFifoOccupancy[source] : 0;
}

bool
TimingVeu::allSourcesReady(uint32_t chunk) const
{
    for (uint8_t index = 0; index < 3; ++index) {
        const auto source = static_cast<VeuSource>(index + 1);
        if (!VeuFunctionalExecutor::sourceRequired(operationInfo.sourceMask,
                                                   source)) continue;
        const auto &fifo = inputFifos[index];
        if (std::none_of(fifo.begin(), fifo.end(),
                         [chunk](const SourceChunk &entry) {
                             return entry.chunk == chunk;
                         })) return false;
    }
    return true;
}

TimingVeu::SourceChunk
TimingVeu::takeSourceChunk(VeuSource source, uint32_t chunk)
{
    auto &fifo = inputFifos[static_cast<uint8_t>(source) - 1];
    const auto found = std::find_if(fifo.begin(), fifo.end(),
        [chunk](const SourceChunk &entry) { return entry.chunk == chunk; });
    if (found == fifo.end()) {
        throw std::runtime_error("TimingVEU source FIFO underflow");
    }
    SourceChunk result = *found;
    fifo.erase(found);
    trace("fifo_pop", chunk, source);
    return result;
}

void
TimingVeu::processResponses()
{
    while (!pendingResponses.empty()) {
        PendingResponse response = std::move(pendingResponses.front());
        pendingResponses.pop_front();
        const auto found = outstanding.find(response.transactionId);
        if (found == outstanding.end()) {
            ++unexpectedResponses;
            throw std::runtime_error("TimingVEU late or unknown response");
        }
        const auto request = found->second.request;
        outstanding.erase(found);
        if (response.isWrite) {
            trace("write_response", request.chunkIndex, VeuSource::None,
                  response.transactionId, request.address);
            completeChunk(request.chunkIndex);
        } else {
            const uint8_t index = static_cast<uint8_t>(request.source) - 1;
            if (outstandingBySource[index] == 0) {
                ++unexpectedResponses;
                throw std::runtime_error("TimingVEU outstanding read underflow");
            }
            --outstandingBySource[index];
            if (inputFifos[index].size() >= activeTiming.fifoDepth) {
                throw std::runtime_error("TimingVEU input FIFO overflow");
            }
            inputFifos[index].push_back({request.chunkIndex, response.data});
            maxFifoOccupancy[index] = std::max<uint64_t>(
                maxFifoOccupancy[index], inputFifos[index].size());
            trace("read_response", request.chunkIndex, request.source,
                  response.transactionId, request.address, &response.data);
            trace("fifo_push", request.chunkIndex, request.source);
        }
    }
}

void
TimingVeu::advancePipelines()
{
    while (!vfuPipeline.empty() && vfuPipeline.front().readyCycle <= modelCycle) {
        ResultToken token = std::move(vfuPipeline.front());
        vfuPipeline.pop_front();
        ++vfuCompleted;
        trace("vfu_done", token.chunk);
        token.readyCycle = modelCycle + activeTiming.vsuLatency;
        vsuPipeline.push_back(std::move(token));
    }
    while (!vsuPipeline.empty() && vsuPipeline.front().readyCycle <= modelCycle) {
        if (vsuPipeline.front().result.writeResult &&
            storeQueue.size() >= activeTiming.fifoDepth) {
            ++vsuQueueStalls;
            break;
        }
        ResultToken token = std::move(vsuPipeline.front());
        vsuPipeline.pop_front();
        trace("vsu_ready", token.chunk);
        if (token.result.writeResult) {
            storeQueue.push_back(std::move(token));
        } else {
            if (csr.mask == 0) ++zeroMaskSkippedWrites;
            completeChunk(token.chunk);
        }
    }
}

void
TimingVeu::acceptVfuInput()
{
    if (nextExecuteChunk >= operationChunkCount) return;
    if (!allSourcesReady(nextExecuteChunk)) {
        ++fifoEmptyStalls;
        return;
    }
    if (modelCycle < nextVfuAcceptCycle) {
        ++vfuIIStalls;
        return;
    }
    VeuFunctionalInput input;
    input.instruction = operationInstruction;
    input.config = csr.config;
    input.scalar = csr.raddr1;
    input.writeMask = csr.mask;
    input.chunkIndex = nextExecuteChunk;
    input.chunkCount = operationChunkCount;
    for (uint8_t index = 0; index < 3; ++index) {
        const auto source = static_cast<VeuSource>(index + 1);
        if (!VeuFunctionalExecutor::sourceRequired(operationInfo.sourceMask,
                                                   source)) continue;
        const VeuVector data = takeSourceChunk(source, nextExecuteChunk).data;
        if (source == VeuSource::Source1) input.source1 = data;
        if (source == VeuSource::Source2) input.source2 = data;
        if (source == VeuSource::Source3) input.source3 = data;
    }
    ResultToken token;
    token.chunk = nextExecuteChunk;
    token.result = functionalExecutor.execute(input);
    token.readyCycle = modelCycle + activeTiming.latency;
    vfuPipeline.push_back(std::move(token));
    ++vfuAccepted;
    maxVfuInFlight = std::max<uint64_t>(maxVfuInFlight, vfuPipeline.size());
    trace("vfu_accept", nextExecuteChunk);
    ++nextExecuteChunk;
    nextVfuAcceptCycle = modelCycle + activeTiming.initiationInterval;
}

uint32_t
TimingVeu::sourceAddress(VeuSource source, uint32_t chunk) const
{
    uint32_t base = 0;
    switch (source) {
      case VeuSource::Source1: base = csr.raddr1; break;
      case VeuSource::Source2: base = csr.raddr2; break;
      case VeuSource::Source3: base = csr.raddr3; break;
      default: break;
    }
    return base + chunk * VeuVectorBytes;
}

bool
TimingVeu::readCanIssue() const
{
    if (currentOutstandingReadCount() >= activeTiming.maxOutstandingReads) {
        return false;
    }
    for (uint8_t index = 0; index < 3; ++index) {
        const auto source = static_cast<VeuSource>(index + 1);
        if (!VeuFunctionalExecutor::sourceRequired(operationInfo.sourceMask,
                                                   source)) continue;
        if (nextReadChunk[index] < operationChunkCount &&
            inputFifos[index].size() + outstandingBySource[index] <
                activeTiming.fifoDepth) return true;
    }
    return false;
}

std::optional<TimingVeuMemoryRequest>
TimingVeu::makeReadRequest()
{
    if (currentOutstandingReadCount() >= activeTiming.maxOutstandingReads) {
        return std::nullopt;
    }
    for (uint8_t offset = 0; offset < 3; ++offset) {
        const uint8_t index = (readRoundRobin + offset) % 3;
        const auto source = static_cast<VeuSource>(index + 1);
        if (!VeuFunctionalExecutor::sourceRequired(operationInfo.sourceMask,
                                                   source)) continue;
        if (nextReadChunk[index] >= operationChunkCount) continue;
        if (inputFifos[index].size() + outstandingBySource[index] >=
            activeTiming.fifoDepth) {
            ++fifoFullStalls;
            continue;
        }
        TimingVeuMemoryRequest request;
        request.transactionId = nextTransactionId++;
        request.operationId = operationId;
        request.chunkIndex = nextReadChunk[index];
        request.source = source;
        request.address = sourceAddress(source, request.chunkIndex);
        request.isWrite = false;
        readRoundRobin = (index + 1) % 3;
        return request;
    }
    return std::nullopt;
}

bool
TimingVeu::issueRequest(const TimingVeuMemoryRequest &request)
{
    if (!memoryRequest) return false;
    outstanding.emplace(request.transactionId, Outstanding{request, false});
    if (!memoryRequest(request)) {
        const auto found = outstanding.find(request.transactionId);
        if (found != outstanding.end() && found->second.responseQueued) {
            throw std::runtime_error("TimingVEU rejected request produced response");
        }
        outstanding.erase(request.transactionId);
        ++retries;
        return false;
    }
    if (request.isWrite) {
        ++memoryWrites;
        if (request.writeStrobe != FullWriteMask) ++maskedWrites;
        trace("write_request", request.chunkIndex, VeuSource::None,
              request.transactionId, request.address, &request.data,
              request.writeStrobe);
    } else {
        const uint8_t index = static_cast<uint8_t>(request.source) - 1;
        ++nextReadChunk[index];
        ++outstandingBySource[index];
        ++memoryReads;
        maxOutstandingReadsSeen = std::max<uint64_t>(
            maxOutstandingReadsSeen, currentOutstandingReadCount());
        trace("read_request", request.chunkIndex, request.source,
              request.transactionId, request.address);
    }
    return true;
}

void
TimingVeu::issueOneMemoryRequest()
{
    if (retryRequest) {
        TimingVeuMemoryRequest request = *retryRequest;
        if (issueRequest(request)) retryRequest.reset();
        return;
    }
    if (!storeQueue.empty()) {
        if (readCanIssue()) {
            ++storePriorityCycles;
            ++readsBlockedByStore;
            trace("store_priority", storeQueue.front().chunk);
        }
        const auto &token = storeQueue.front();
        TimingVeuMemoryRequest request;
        request.transactionId = nextTransactionId++;
        request.operationId = operationId;
        request.chunkIndex = token.chunk;
        request.address = csr.waddr +
            (operationInfo.reduction ? 0 : token.chunk * VeuVectorBytes);
        request.isWrite = true;
        request.writeStrobe = token.result.writeStrobe;
        request.data = token.result.data;
        if (issueRequest(request)) {
            storeQueue.pop_front();
        } else {
            retryRequest = request;
            // retryRequest now owns this token. Keeping it in storeQueue
            // would issue the same write again after the retry succeeds.
            storeQueue.pop_front();
        }
        return;
    }
    auto request = makeReadRequest();
    if (!request) return;
    if (!issueRequest(*request)) retryRequest = *request;
}

void
TimingVeu::completeMemoryRead(uint64_t transactionId, const VeuVector &data)
{
    const auto found = outstanding.find(transactionId);
    if (found == outstanding.end() || found->second.request.isWrite ||
        found->second.responseQueued) {
        ++unexpectedResponses;
        throw std::runtime_error("TimingVEU unknown, duplicate, or mismatched read response");
    }
    found->second.responseQueued = true;
    pendingResponses.push_back({transactionId, false, data});
}

void
TimingVeu::completeMemoryWrite(uint64_t transactionId)
{
    const auto found = outstanding.find(transactionId);
    if (found == outstanding.end() || !found->second.request.isWrite ||
        found->second.responseQueued) {
        ++unexpectedResponses;
        throw std::runtime_error("TimingVEU unknown, duplicate, or mismatched write response");
    }
    found->second.responseQueued = true;
    pendingResponses.push_back({transactionId, true, {}});
}

void
TimingVeu::completeChunk(uint32_t chunk)
{
    (void)chunk;
    ++completedChunkCount;
    ++processedChunks;
}

bool
TimingVeu::quiescent() const
{
    if (!outstanding.empty() || !pendingResponses.empty() || retryRequest ||
        !vfuPipeline.empty() || !vsuPipeline.empty() || !storeQueue.empty()) {
        return false;
    }
    return std::all_of(inputFifos.begin(), inputFifos.end(),
                       [](const auto &fifo) { return fifo.empty(); });
}

void
TimingVeu::enterDrainingIfDone()
{
    if (currentState == State::Running &&
        completedChunkCount == operationChunkCount && quiescent()) {
        currentState = State::Draining;
        drainReadyCycle = modelCycle + timing.finishCycles;
    }
    if (currentState == State::Draining && modelCycle >= drainReadyCycle &&
        quiescent()) {
        completeOperation();
    }
}

void
TimingVeu::completeOperation()
{
    trace("status_clear");
    statusBusy = false;
    csr.status &= ~1u;
    trace("lock_finish");
    lockActive = false;
    trace("operation_finish");
    internalActive = false;
    currentState = State::Idle;
    ++completedOperations;
}

void
TimingVeu::advanceOperation()
{
    if (!internalActive) return;
    processResponses();
    advancePipelines();
    if (currentState == State::Running) {
        acceptVfuInput();
        issueOneMemoryRequest();
        if (currentOutstandingReadCount()) ++loadWaitCycles;
        if (!vfuPipeline.empty()) ++executeCycles;
        if (!storeQueue.empty() ||
            std::any_of(outstanding.begin(), outstanding.end(),
                [](const auto &entry) { return entry.second.request.isWrite; })) {
            ++storeWaitCycles;
        }
    }
    enterDrainingIfDone();
}

void
TimingVeu::trace(const char *event, int32_t chunk, VeuSource source,
                 uint64_t transactionId, Addr address, const VeuVector *data,
                 uint32_t strobe, const std::string &detail)
{
    if (!traceStream) return;
    std::ostringstream dataText;
    if (data) {
        dataText << std::hex << std::setfill('0');
        for (auto it = data->rbegin(); it != data->rend(); ++it) {
            dataText << std::setw(2) << static_cast<unsigned>(*it);
        }
    }
    traceStream << modelCycle << ',' << event << ",0,0,"
                << VeuFunctionalExecutor::instructionName(operationInstruction)
                << ',' << csr.vlen << ',' << effectiveVeuLengthAtStart(csr.vlen)
                << ',' << chunk << ',' << static_cast<unsigned>(source) << ','
                << transactionId << ",0x" << std::hex << address << ','
                << dataText.str() << ",0x" << strobe << std::dec << ','
                << inputFifos[0].size() << ',' << inputFifos[1].size() << ','
                << inputFifos[2].size() << ',' << currentOutstandingReadCount()
                << ',' << statusBusy << ',' << lockActive << ',' << detail
                << '\n';
    traceStream.flush();
}

void
TimingVeu::clock(const VeuRequest &request)
{
    ++modelCycle;
    if (internalActive) ++busyCycles;
    if (statusBusy) ++statusActiveCycles;
    if (lockActive) ++lockActiveCycles;
    advanceOperation();

    switch (controlState) {
      case ControlState::Idle:
        if (request.hasTransaction()) acceptRequest(request);
        break;
      case ControlState::Respond:
        ++responses;
        controlState = ControlState::Recovery;
        break;
      case ControlState::Recovery:
        if (request.hasTransaction()) acceptRequest(request);
        else controlState = ControlState::Idle;
        break;
    }
}

} // namespace brs
} // namespace gem5
