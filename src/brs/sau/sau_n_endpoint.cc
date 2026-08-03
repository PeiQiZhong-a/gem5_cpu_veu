#include "brs/sau/sau_n_endpoint.hh"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace gem5::brs
{

SauNEndpoint::SauNEndpoint(
    DutKuiMemoryModel &memory,
    uint32_t dataMemoryBase,
    uint64_t dataMemorySize)
  : memory(memory), dataMemoryBase(dataMemoryBase),
    dataMemorySize(dataMemorySize),
    configState(dataMemoryBase, dataMemorySize)
{
    reset();
}

void
SauNEndpoint::reset()
{
    // The model holds a non-owning reference to memoryView. Destroy it first.
    model.reset();
    memoryView.reset();
    configState.reset();
    currentState = State::Idle;
    activeRequest = {};
    responseReg = {};
    storedMemoryResponse = {};
    cycle = 0;
    operationStarts = 0;
    operationCompletes = 0;
    modelTicks = 0;
    currentRoiStartCycle = 0;
    currentRoiEndCycle = 0;
    traceReg = {};
    lastModelCycleValid = false;
    lastCycle = {};
}

void
SauNEndpoint::setTraceEnabled(bool enabled)
{
    traceIsEnabled = enabled;
    if (!enabled) {
        traceReg = {};
    }
}

void
SauNEndpoint::clock(const SauRequest &request)
{
    clockTick(request, storedMemoryResponse);
    storedMemoryResponse = {};
}

void
SauNEndpoint::clockMemory(const SauMemoryResponse &response)
{
    storedMemoryResponse = response;
}

SauMemoryOutput
SauNEndpoint::evaluateMemory() const
{
    SauMemoryOutput output;
    if (currentState == State::Starting) {
        output.crossbarStart = true;
    } else if (currentState == State::Finishing) {
        output.crossbarDone = true;
    }
    return output;
}

SauInstruction
SauNEndpoint::setInstruction(uint8_t slot)
{
    switch (slot) {
      case 1:
        return SauInstruction::Set1;
      case 2:
        return SauInstruction::Set2;
      case 3:
        return SauInstruction::Set3;
      case 4:
        return SauInstruction::Set4;
      default:
        return SauInstruction::Unknown;
    }
}

bool
SauNEndpoint::isMget4Lsb(const SauRequest &request)
{
    return request.csrRead && !request.csrWrite &&
        request.csrAddr == SauCsrBase + 6;
}

bool
SauNEndpoint::sameRequest(
    const SauRequest &lhs, const SauRequest &rhs)
{
    return lhs.csrAddr == rhs.csrAddr &&
        lhs.csrRead == rhs.csrRead &&
        lhs.csrWrite == rhs.csrWrite &&
        lhs.writeType == rhs.writeType &&
        lhs.writeData == rhs.writeData &&
        lhs.veStart == rhs.veStart;
}

std::string
SauNEndpoint::payloadSummary(
    const std::array<uint64_t, 4> &payloads) const
{
    std::ostringstream text;
    text << std::hex << "[";
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        if (index != 0) {
            text << ",";
        }
        text << "0x" << payloads[index];
    }
    text << "]";
    return text.str();
}

std::string
SauNEndpoint::configSummary(const SauNResolvedConfig &config) const
{
    std::ostringstream text;
    text << std::hex
         << "input_base=0x" << config.abi.inputBase
         << " weight_base=0x" << config.abi.weightBase
         << " bias_base=0x" << config.abi.biasBase
         << " output_base=0x" << config.abi.outputBase
         << std::dec
         << " n=" << config.pipeline.im2col.n
         << " c=" << config.pipeline.im2col.c
         << " h=" << config.pipeline.im2col.h
         << " w=" << config.pipeline.im2col.w
         << " out_h=" << config.pipeline.im2col.outH
         << " out_w=" << config.pipeline.im2col.outW
         << " out_c=" << config.pipeline.outChannels
         << " stride=" << config.pipeline.im2col.strideH
         << " padding=" << config.pipeline.im2col.padTop
         << " cutbit=" << config.pipeline.cutbit;
    return text.str();
}

void
SauNEndpoint::handleHcRequest(const SauRequest &request)
{
    if (isMget4Lsb(request)) {
        // The current gem5 integration keeps msetins4 blocking until sau_n
        // drains, so an accepted mgetins4lsb in this endpoint is idle.  Keep
        // the read visible to the CPU so toolchain-style pre/post polls can
        // be represented without changing the endpoint's blocking contract.
        responseReg = {true, 0};
        currentState = State::Responding;
        return;
    }

    if (request.csrRead || !request.csrWrite ||
        request.writeType != static_cast<uint8_t>(SauWriteType::Set) ||
        request.csrAddr < SauCsrBase ||
        request.csrAddr >= SauCsrBase + 8 ||
        ((request.csrAddr - SauCsrBase) & 1) != 0) {
        throw std::invalid_argument(
            "SauNEndpoint accepts only four ordered msetins1..4 writes");
    }

    const uint8_t slot = static_cast<uint8_t>(
        (request.csrAddr - SauCsrBase) / 2 + 1);
    const SauInstruction operation = setInstruction(slot);
    if (operation == SauInstruction::Unknown) {
        throw std::invalid_argument(
            "SauNEndpoint received an unsupported msetins slot");
    }

    const SauNWriteResult result = configState.write(
        operation, request.writeData);
    if (!result.accepted) {
        auto payloads = configState.csrConfig().shadowWords();
        payloads[slot - 1] = request.writeData;
        throw std::logic_error(
            std::string("SauNEndpoint rejected CSR transaction: ") +
            sauNConfigErrorString(result.error) + ": " + result.detail +
            " payloads=" + payloadSummary(payloads));
    }

    activeRequest = request;
    if (traceIsEnabled) {
        if (slot == 1) {
            traceReg = {};
        }
        traceReg.payloads[slot - 1] = request.writeData;
    }

    if (slot < 4) {
        responseReg = {true, 0};
        currentState = State::Responding;
        return;
    }

    const SauNResolvedConfig *config = configState.activeConfig();
    if (!result.committed || config == nullptr) {
        throw std::logic_error(
            "SauNEndpoint committed without an active configuration");
    }
    try {
        startOperation(*config);
    } catch (const std::exception &error) {
        throw std::logic_error(
            std::string("SauNEndpoint failed to construct streaming model: ") +
            error.what() + " config=" + configSummary(*config) +
            " payloads=" + payloadSummary(config->payloads));
    }
}

void
SauNEndpoint::recordConfigTrace(const SauNResolvedConfig &config)
{
    if (!traceIsEnabled) {
        return;
    }
    traceReg.config = config;
    traceReg.hasConfig = true;
    traceReg.stateHistory.clear();
    traceReg.completionReason.clear();
}

void
SauNEndpoint::startOperation(const SauNResolvedConfig &config)
{
    // Preserve the non-owning backing lifetime contract across operations.
    model.reset();
    memoryView.reset();
    const SauNTensorBaseAddresses bases{
        config.abi.inputBase,
        config.abi.weightBase,
        config.abi.biasBase,
        config.abi.outputBase,
    };
    memoryView = std::make_unique<SauNMemoryView>(
        config.pipeline, memory, bases);
    model = std::make_unique<sau_n::StreamingConvPipelineModel>(
        config.pipeline, *memoryView);
    currentState = State::Starting;
    ++operationStarts;
    modelTicks = 0;
    currentRoiStartCycle = cycle;
    currentRoiEndCycle = 0;
    lastModelCycleValid = false;
    lastCycle = {};
    recordConfigTrace(config);
}

void
SauNEndpoint::clockTick(
    const SauRequest &request,
    const SauMemoryResponse &memoryResponse)
{
    ++cycle;
    if (traceIsEnabled) {
        traceReg.stateHistory.push_back(
            static_cast<uint8_t>(currentState));
    }
    if (memoryResponse.valid) {
        throw std::logic_error(
            "SauNEndpoint LocalScratchpadBacking received an external SRAM response");
    }

    const bool responseWasValid = responseReg.valid;
    if (responseWasValid) {
        responseReg = {};
        if (currentState == State::Responding) {
            currentState = State::Recovery;
        }
        return;
    }

    if (currentState == State::Recovery) {
        if (!request.hasTransaction()) {
            currentState = State::Idle;
        } else if (isMget4Lsb(request)) {
            currentState = State::Idle;
            handleHcRequest(request);
        }
        return;
    }

    if (currentState == State::Idle) {
        if (request.hasTransaction()) {
            handleHcRequest(request);
        }
        return;
    }

    if (currentState == State::Starting) {
        if (request.hasTransaction() &&
            !sameRequest(request, activeRequest)) {
            throw std::logic_error(
                "SauNEndpoint received a different HC request while starting");
        }
        currentState = State::Running;
        return;
    }

    if (currentState == State::Running) {
        if (request.hasTransaction() &&
            !sameRequest(request, activeRequest)) {
            throw std::logic_error(
                "SauNEndpoint received a new HC request while running");
        }
        if (!model) {
            throw std::logic_error(
                "SauNEndpoint is running without a streaming model");
        }
        lastCycle = model->tick();
        lastModelCycleValid = true;
        ++modelTicks;
        if (lastCycle.drained) {
            currentState = State::Finishing;
        }
        return;
    }

    // Finishing is a release pulse only.  The model was drained on the
    // preceding running edge, so completion is published one edge after the
    // crossbarDone pulse becomes visible.
    if (currentState == State::Finishing) {
        if (request.hasTransaction() &&
            !sameRequest(request, activeRequest)) {
            throw std::logic_error(
                "SauNEndpoint received a different HC request while finishing");
        }
        if (!configState.completeActiveOperation()) {
            throw std::logic_error(
                "SauNEndpoint finished without a busy active configuration");
        }
        ++operationCompletes;
        currentRoiEndCycle = cycle;
        if (traceIsEnabled) {
            traceReg.completionReason = "drained_then_crossbar_done";
        }
        responseReg = {true, 0};
        currentState = State::Responding;
    }
}

} // namespace gem5::brs
