#include "sau_n/streaming_conv_pipeline_model.hh"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "sau_n/im2col_address.hh"
#include "sau_n/sau_generators.hh"

namespace gem5::sau_n
{
namespace
{

bool
anySpadRequest(const SramRequest &request)
{
    return std::any_of(
        request.valid.begin(), request.valid.end(),
        [](bool valid) { return valid; });
}

uint64_t
requestCount(const SramRequest &request)
{
    return static_cast<uint64_t>(std::count(
        request.valid.begin(), request.valid.end(), true));
}

uint64_t
responseCount(const SramResponse &response)
{
    return static_cast<uint64_t>(std::count(
        response.valid.begin(), response.valid.end(), true));
}

} // anonymous namespace

StreamingConvPipelineModel::StreamingConvPipelineModel(
    const PipelineResolvedConfig &config,
    const OutputReadyConfig &ready)
    : resolved(config), dimensions(validateStreamingConfig(resolved)),
      readyConfig(ready),
      producer(resolved, PipelinedIm2ColMemoryMode::SharedOneCycle),
      sauModel(InputProtocol)
{
    initializeModel(true);
}

StreamingConvPipelineModel::StreamingConvPipelineModel(
    const PipelineResolvedConfig &config,
    ScratchpadBacking &backing,
    const OutputReadyConfig &ready)
    : resolved(config), dimensions(validateStreamingConfig(resolved)),
      readyConfig(ready),
      producer(resolved, PipelinedIm2ColMemoryMode::SharedOneCycle),
      sharedScratchpad(backing),
      sauModel(InputProtocol)
{
    initializeModel(false);
}

void
StreamingConvPipelineModel::initializeModel(bool preload)
{
    validateOutputReady(readyConfig);
    resolved.sharedSpad = dimensions.sharedSpad;
    if (preload) {
        preloadSharedScratchpad();
    }
    initializeBBuffers();
    if (dimensions.expectedOutputs >
        std::numeric_limits<std::size_t>::max()) {
        throw std::overflow_error(
            "streaming output count exceeds host container size");
    }
    collectedOutputs.resize(
        static_cast<std::size_t>(dimensions.expectedOutputs));
    outputWritten.resize(
        static_cast<std::size_t>(dimensions.expectedOutputs));
}

void
StreamingConvPipelineModel::preloadSharedScratchpad()
{
    sharedScratchpad.clear();
    const ChwAddressMapper mapper(resolved.im2col);
    for (uint64_t n = 0; n < resolved.im2col.n; ++n) {
        for (uint64_t c = 0; c < resolved.im2col.c; ++c) {
            for (uint64_t h = 0; h < resolved.im2col.h; ++h) {
                for (uint64_t w = 0; w < resolved.im2col.w; ++w) {
                    const auto location = mapper.locate(n, c, h, w);
                    sharedScratchpad.write(
                        location.bank, location.row,
                        tbActValueV1(n, c, h, w));
                }
            }
        }
    }

    for (uint64_t k = 0; k < dimensions.k; ++k) {
        const uint64_t kernelIndex =
            k % (SauKernelHeight * SauKernelWidth);
        const uint64_t c = k / (SauKernelHeight * SauKernelWidth);
        const uint64_t kh = kernelIndex / SauKernelWidth;
        const uint64_t kw = kernelIndex % SauKernelWidth;
        for (uint64_t oc = 0; oc < resolved.outChannels; ++oc) {
            const auto location = bAddress(resolved, dimensions, k, oc);
            sharedScratchpad.write(
                location.bank, location.row,
                static_cast<uint8_t>(
                    weightValue(
                        resolved.weightGenerator, oc, c, kh, kw)));
        }
    }

    for (uint64_t oc = 0; oc < resolved.outChannels; ++oc) {
        const uint16_t raw =
            static_cast<uint16_t>(biasValue(resolved.biasGenerator, oc));
        for (uint64_t byte = 0; byte < 2; ++byte) {
            const auto location = cAddress(
                resolved, dimensions, oc, byte);
            sharedScratchpad.write(
                location.bank, location.row,
                static_cast<uint8_t>(raw >> (byte * 8)));
        }
    }
}

uint16_t
StreamingConvPipelineModel::expectedBMask() const
{
    return resolved.outChannels == SauColumns ?
        uint16_t{0xffff} :
        static_cast<uint16_t>((uint32_t{1} << resolved.outChannels) - 1);
}

bool
StreamingConvPipelineModel::biasesReady() const
{
    for (uint64_t column = 0;
         column < resolved.outChannels; ++column) {
        if (biasReadyMasks[column] != 0x3) {
            return false;
        }
    }
    return true;
}

void
StreamingConvPipelineModel::applyCResponses(
    const SramResponse &response)
{
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        const auto tag = cInFlight[bank];
        if (!tag.valid) {
            continue;
        }
        if (!response.valid[bank] || bank >= resolved.outChannels ||
            tag.byte >= 2) {
            throw std::logic_error(
                "C response does not match its in-flight tag");
        }
        const uint8_t bit = static_cast<uint8_t>(1U << tag.byte);
        if (biasReadyMasks[bank] & bit) {
            throw std::logic_error("duplicate C bias-byte response");
        }
        biasBytes[bank][tag.byte] = response.data[bank];
        biasReadyMasks[bank] |= bit;
        counters.spadReadResponsesC = checkedAdd(
            counters.spadReadResponsesC, 1, "C read response count");
        if (biasReadyMasks[bank] == 0x3) {
            const uint16_t raw =
                uint16_t{biasBytes[bank][0]} |
                (uint16_t{biasBytes[bank][1]} << 8);
            const int32_t signedValue = raw < 0x8000 ?
                static_cast<int32_t>(raw) :
                static_cast<int32_t>(raw) - 65536;
            loadedBiases[bank] = static_cast<int16_t>(signedValue);
        }
    }
    cInFlight = {};
}

StreamingConvPipelineModel::CRequestContext
StreamingConvPipelineModel::buildCRequest() const
{
    CRequestContext context;
    for (uint64_t column = 0;
         column < resolved.outChannels; ++column) {
        if (biasReadyMasks[column] == 0x3) {
            continue;
        }
        const uint8_t byte =
            (biasReadyMasks[column] & 0x1) == 0 ? 0 : 1;
        const auto address = cAddress(
            resolved, dimensions, column, byte);
        context.request.valid[address.bank] = true;
        context.request.address[address.bank] =
            static_cast<uint16_t>(address.row);
        context.bytes[address.bank] = byte;
    }
    return context;
}

void
StreamingConvPipelineModel::installCInFlight(
    const CRequestContext &context, const SramRequest &grant)
{
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        if (!grant.valid[bank]) {
            continue;
        }
        if (!context.request.valid[bank] ||
            context.request.address[bank] != grant.address[bank]) {
            throw std::logic_error(
                "C grant is not a subset of its request context");
        }
        cInFlight[bank] = {true, context.bytes[bank]};
    }
}

SramRequest
StreamingConvPipelineModel::buildDRequest() const
{
    SramRequest request;
    if (dQueue.empty()) {
        return request;
    }
    const auto &head = dQueue.front();
    for (uint64_t column = 0;
         column < resolved.outChannels; ++column) {
        if ((head.pendingMask & (uint16_t{1} << column)) == 0) {
            continue;
        }
        const auto address = dAddress(
            resolved, dimensions, head.coordinate.n,
            head.coordinate.oh, head.coordinate.ow, column);
        request.valid[address.bank] = true;
        request.address[address.bank] =
            static_cast<uint16_t>(address.row);
    }
    return request;
}

void
StreamingConvPipelineModel::applyDWrites(
    std::deque<DPendingRow> &working, const SramRequest &grant)
{
    if (!anySpadRequest(grant)) {
        return;
    }
    if (working.empty()) {
        throw std::logic_error("D grant requires a pending row");
    }
    auto &head = working.front();
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        if (!grant.valid[bank]) {
            continue;
        }
        if (bank >= resolved.outChannels ||
            (head.pendingMask & (uint16_t{1} << bank)) == 0) {
            throw std::logic_error(
                "D grant does not target a pending output lane");
        }
        const auto address = dAddress(
            resolved, dimensions, head.coordinate.n,
            head.coordinate.oh, head.coordinate.ow, bank);
        if (grant.address[bank] != address.row) {
            throw std::logic_error("D grant address mismatch");
        }
        const uint64_t index = outputIndex(head.coordinate, bank);
        if (outputWritten[static_cast<std::size_t>(index)]) {
            throw std::logic_error("duplicate streaming D write");
        }
        sharedScratchpad.write(
            address.bank, address.row,
            static_cast<uint8_t>(head.values[bank]));
        outputWritten[static_cast<std::size_t>(index)] = true;
        head.pendingMask &= ~(uint16_t{1} << bank);
        counters.outputElements = checkedAdd(
            counters.outputElements, 1,
            "streaming output element count");
    }
    if (head.pendingMask == 0) {
        working.pop_front();
        counters.outputRows = checkedAdd(
            counters.outputRows, 1, "streaming output row count");
    }
}

void
StreamingConvPipelineModel::enqueueDOutput(
    std::deque<DPendingRow> &working,
    const SauCycleObservation &observation)
{
    if (!observation.rowScoreValid) {
        return;
    }
    if (!activeEntry ||
        observation.rowSequence != tileOutputRows ||
        observation.rowSequence >= activeEntry->payload.validRows) {
        throw std::logic_error(
            "streaming SA output row does not match active tile");
    }
    if (working.size() >= dimensions.sharedSpad.dPendingRows) {
        throw std::logic_error("D pending queue overflow");
    }
    DPendingRow entry;
    entry.coordinate =
        activeEntry->payload.coordinates[observation.rowSequence];
    entry.validMask = expectedBMask();
    entry.pendingMask = entry.validMask;
    for (uint64_t column = 0;
         column < resolved.outChannels; ++column) {
        const uint16_t slot = observation.outputSlots[column];
        const int32_t signedSlot = slot < 0x8000 ?
            static_cast<int32_t>(slot) :
            static_cast<int32_t>(slot) - 65536;
        if (signedSlot < -128 || signedSlot > 127) {
            throw std::logic_error(
                "streaming SA output is not a signed INT8 value");
        }
        const int8_t value = static_cast<int8_t>(signedSlot);
        if (signExtendedInt8Slot(value) != slot) {
            throw std::logic_error(
                "streaming SA output is not canonical sign extension");
        }
        entry.values[column] = value;
    }
    working.push_back(entry);
    tileOutputRows = checkedAdd(
        tileOutputRows, 1, "streaming tile output row count");
}

void
StreamingConvPipelineModel::rebuildOutputsFromD()
{
    for (uint64_t n = 0; n < resolved.im2col.n; ++n) {
        for (uint64_t column = 0;
             column < resolved.outChannels; ++column) {
            for (uint64_t oh = 0; oh < resolved.im2col.outH; ++oh) {
                for (uint64_t ow = 0; ow < resolved.im2col.outW; ++ow) {
                    const StreamingSpatialCoordinate coordinate{
                        true, n, oh, ow};
                    const uint64_t index =
                        outputIndex(coordinate, column);
                    if (!outputWritten[static_cast<std::size_t>(index)]) {
                        throw std::logic_error(
                            "missing streaming D write at drain");
                    }
                    const auto address = dAddress(
                        resolved, dimensions, n, oh, ow, column);
                    collectedOutputs[static_cast<std::size_t>(index)] =
                        signedInt8(sharedScratchpad.read(
                            address.bank, address.row));
                }
            }
        }
    }
}

void
StreamingConvPipelineModel::configureBChunk(
    uint64_t buffer, uint64_t baseK)
{
    if (buffer >= bBuffers.size()) {
        throw std::out_of_range("B buffer index exceeds two buffers");
    }
    auto &target = bBuffers[buffer];
    target = {};
    target.entries.resize(
        static_cast<std::size_t>(dimensions.sharedSpad.bBufferDepth));
    if (baseK >= dimensions.k) {
        return;
    }
    target.valid = true;
    target.weightTile = 0;
    target.chunkBaseK = baseK;
    target.chunkLength = std::min(
        dimensions.sharedSpad.bBufferDepth, dimensions.k - baseK);
    for (uint64_t slot = 0; slot < target.chunkLength; ++slot) {
        auto &entry = target.entries[static_cast<std::size_t>(slot)];
        entry.valid = true;
        entry.weightTile = 0;
        entry.globalK = baseK + slot;
    }
}

void
StreamingConvPipelineModel::initializeBBuffers()
{
    fullBResident = dimensions.sharedSpad.weightReuse &&
        checkedMultiply(
            dimensions.sharedSpad.bBufferDepth, 2,
            "combined B buffer capacity") >= dimensions.k;
    configureBChunk(0, 0);
    const uint64_t secondBase = bBuffers[0].chunkLength;
    configureBChunk(1, secondBase);
    nextBChunkBase = checkedAdd(
        secondBase, bBuffers[1].chunkLength, "next B chunk base");
    activeBBuffer = 0;
    nextExpectedK = 0;
}

bool
StreamingConvPipelineModel::bBufferComplete(uint64_t buffer) const
{
    if (buffer >= bBuffers.size() || !bBuffers[buffer].valid) {
        return false;
    }
    const auto &value = bBuffers[buffer];
    for (uint64_t slot = 0; slot < value.chunkLength; ++slot) {
        const auto &entry = value.entries[static_cast<std::size_t>(slot)];
        if (!entry.valid || entry.weightTile != value.weightTile ||
            entry.globalK != value.chunkBaseK + slot ||
            entry.readyMask != expectedBMask()) {
            return false;
        }
    }
    return true;
}

uint64_t
StreamingConvPipelineModel::bReadyEntryCount(
    const std::array<BBuffer, 2> &buffers) const
{
    uint64_t ready = 0;
    for (const auto &buffer : buffers) {
        if (!buffer.valid) {
            continue;
        }
        for (uint64_t slot = 0; slot < buffer.chunkLength; ++slot) {
            const auto &entry =
                buffer.entries[static_cast<std::size_t>(slot)];
            if (entry.valid && entry.weightTile == buffer.weightTile &&
                entry.globalK == buffer.chunkBaseK + slot &&
                entry.readyMask == expectedBMask()) {
                ready = checkedAdd(
                    ready, 1, "B ready-entry occupancy");
            }
        }
    }
    return ready;
}

bool
StreamingConvPipelineModel::bAllResidentReady() const
{
    if (!fullBResident) {
        return false;
    }
    uint64_t nextK = 0;
    for (uint64_t index = 0; index < bBuffers.size(); ++index) {
        const auto &buffer = bBuffers[index];
        if (!buffer.valid) {
            continue;
        }
        if (buffer.chunkBaseK != nextK) {
            return false;
        }
        if (!bBufferComplete(index)) {
            return false;
        }
        nextK += buffer.chunkLength;
    }
    return nextK == dimensions.k;
}

bool
StreamingConvPipelineModel::bBufferCovers(
    uint64_t buffer, uint64_t k) const
{
    if (buffer >= bBuffers.size() || !bBuffers[buffer].valid) {
        return false;
    }
    const auto &value = bBuffers[buffer];
    return value.weightTile == 0 && k >= value.chunkBaseK &&
        k - value.chunkBaseK < value.chunkLength;
}

bool
StreamingConvPipelineModel::bEntryReady(
    uint64_t buffer, uint64_t k) const
{
    if (!bBufferCovers(buffer, k)) {
        return false;
    }
    const auto &value = bBuffers[buffer];
    const auto &entry = value.entries[
        static_cast<std::size_t>(k - value.chunkBaseK)];
    return entry.valid && entry.weightTile == value.weightTile &&
        entry.globalK == k && entry.readyMask == expectedBMask();
}

const StreamingConvPipelineModel::BBufferEntry &
StreamingConvPipelineModel::activeBEntry(uint64_t k) const
{
    if (!bEntryReady(activeBBuffer, k)) {
        throw std::logic_error(
            "active B buffer does not contain a ready expected K entry");
    }
    const auto &buffer = bBuffers[activeBBuffer];
    return buffer.entries[static_cast<std::size_t>(
        k - buffer.chunkBaseK)];
}

void
StreamingConvPipelineModel::switchActiveB(uint64_t nextBuffer)
{
    if (nextBuffer >= bBuffers.size() || nextBuffer == activeBBuffer ||
        !bBufferComplete(nextBuffer) ||
        !bBufferCovers(nextBuffer, nextExpectedK)) {
        throw std::logic_error("invalid B buffer switch");
    }
    const uint64_t oldActive = activeBBuffer;
    activeBBuffer = nextBuffer;
    counters.bBufferSwitches = checkedAdd(
        counters.bBufferSwitches, 1, "B buffer switch count");
    if (!fullBResident) {
        configureBChunk(oldActive, nextBChunkBase);
        nextBChunkBase = checkedAdd(
            nextBChunkBase, bBuffers[oldActive].chunkLength,
            "next B chunk base");
    }
}

void
StreamingConvPipelineModel::refreshActiveB()
{
    if (bBufferCovers(activeBBuffer, nextExpectedK)) {
        return;
    }
    const uint64_t other = 1 - activeBBuffer;
    if (bBufferComplete(other) &&
        bBufferCovers(other, nextExpectedK)) {
        switchActiveB(other);
    }
}

void
StreamingConvPipelineModel::resetBForNextTile()
{
    nextExpectedK = 0;
    if (fullBResident) {
        activeBBuffer = bBufferCovers(0, 0) ? 0 : 1;
        return;
    }
    configureBChunk(0, 0);
    const uint64_t secondBase = bBuffers[0].chunkLength;
    configureBChunk(1, secondBase);
    nextBChunkBase = checkedAdd(
        secondBase, bBuffers[1].chunkLength, "next B chunk base");
    activeBBuffer = 0;
}

void
StreamingConvPipelineModel::applyBResponses(
    std::array<BBuffer, 2> &working,
    const SramResponse &response)
{
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        const auto tag = bInFlight[bank];
        if (!tag.valid) {
            if (response.valid[bank] && !aInFlight.valid[bank] &&
                !cInFlight[bank].valid) {
                throw std::logic_error(
                    "scratchpad response has no A/B/C owner");
            }
            continue;
        }
        if (!response.valid[bank] || tag.buffer >= working.size()) {
            throw std::logic_error(
                "B response does not match its in-flight tag");
        }
        auto &buffer = working[tag.buffer];
        if (!buffer.valid || tag.slot >= buffer.chunkLength) {
            throw std::logic_error(
                "B response targets an invalid buffer slot");
        }
        auto &entry =
            buffer.entries[static_cast<std::size_t>(tag.slot)];
        if (!entry.valid || entry.globalK != tag.globalK ||
            entry.readyMask & (uint16_t{1} << bank)) {
            throw std::logic_error(
                "B response identity or ready mask mismatch");
        }
        const bool wasComplete = entry.readyMask == expectedBMask();
        entry.weights[bank] = signedInt8(response.data[bank]);
        entry.readyMask |= uint16_t{1} << bank;
        counters.spadReadResponsesB = checkedAdd(
            counters.spadReadResponsesB, 1, "B read response count");
        if (!wasComplete && entry.readyMask == expectedBMask()) {
            counters.bBufferFillVectors = checkedAdd(
                counters.bBufferFillVectors, 1,
                "B buffer fill vector count");
        }
    }
    bInFlight = {};
}

StreamingConvPipelineModel::BRequestContext
StreamingConvPipelineModel::buildBRequest(
    const std::array<BBuffer, 2> &working) const
{
    std::array<uint64_t, 2> order = {
        activeBBuffer, 1 - activeBBuffer};
    for (const uint64_t bufferIndex : order) {
        const auto &buffer = working[bufferIndex];
        if (!buffer.valid) {
            continue;
        }
        for (uint64_t slot = 0; slot < buffer.chunkLength; ++slot) {
            const auto &entry =
                buffer.entries[static_cast<std::size_t>(slot)];
            if (entry.readyMask == expectedBMask()) {
                continue;
            }
            BRequestContext context;
            context.valid = true;
            context.buffer = bufferIndex;
            context.slot = slot;
            context.globalK = entry.globalK;
            for (uint64_t column = 0;
                 column < resolved.outChannels; ++column) {
                if (entry.readyMask & (uint16_t{1} << column)) {
                    continue;
                }
                const auto address = bAddress(
                    resolved, dimensions, entry.globalK, column);
                context.request.valid[address.bank] = true;
                context.request.address[address.bank] =
                    static_cast<uint16_t>(address.row);
            }
            return context;
        }
    }
    return {};
}

void
StreamingConvPipelineModel::installBInFlight(
    const BRequestContext &context, const SramRequest &grant)
{
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        if (!grant.valid[bank]) {
            continue;
        }
        if (!context.valid || !context.request.valid[bank] ||
            context.request.address[bank] != grant.address[bank]) {
            throw std::logic_error(
                "B grant is not a subset of its request context");
        }
        bInFlight[bank] = {
            true, context.buffer, context.slot, context.globalK};
    }
}

bool
StreamingConvPipelineModel::bLaunchReady(
    const StreamingFifoEntry &head) const
{
    if (!biasesReady() ||
        !head.tag.tileFirst || head.tag.kIndex != 0) {
        return false;
    }
    if (fullBResident) {
        return bAllResidentReady();
    }
    return bEntryReady(activeBBuffer, 0);
}

bool
StreamingConvPipelineModel::bInputReady(
    const StreamingFifoEntry &head) const
{
    return head.tag.kIndex == nextExpectedK &&
        bEntryReady(activeBBuffer, nextExpectedK);
}

void
StreamingConvPipelineModel::consumeBInput(uint64_t k)
{
    if (k != nextExpectedK) {
        throw std::logic_error("B consumption K sequence diverged");
    }
    activeBEntry(k);
    counters.bBufferConsumedVectors = checkedAdd(
        counters.bBufferConsumedVectors, 1,
        "B buffer consumed vector count");
    counters.bBufferHitVectors = checkedAdd(
        counters.bBufferHitVectors, 1, "B buffer hit vector count");
    if (fullBResident && activeTile != 0) {
        counters.weightReuseHits = checkedAdd(
            counters.weightReuseHits, 1, "weight reuse hit count");
    }
    nextExpectedK = checkedAdd(
        nextExpectedK, 1, "next expected B K");
    if (nextExpectedK == dimensions.k) {
        if (activeTile + 1 < dimensions.expectedTiles) {
            resetBForNextTile();
        }
        return;
    }
    if (!bBufferCovers(activeBBuffer, nextExpectedK)) {
        const uint64_t other = 1 - activeBBuffer;
        if (bBufferComplete(other) &&
            bBufferCovers(other, nextExpectedK)) {
            switchActiveB(other);
        }
    }
}

SauCycleInputs
StreamingConvPipelineModel::buildSauInputs(
    const StreamingConsumerDecision &decision,
    const StreamingFifoEntry &head, bool outputGrant) const
{
    SauCycleInputs inputs;
    inputs.outputGrant = outputGrant;
    if (decision.launch) {
        if (!activeEntry) {
            throw std::logic_error(
                "streaming launch requires active tile metadata");
        }
        inputs.insValid = true;
        inputs.config.calcCycles = dimensions.k;
        inputs.config.validRows = activeEntry->payload.validRows;
        inputs.config.validColumns = resolved.outChannels;
        inputs.config.cutbit = resolved.cutbit;
        for (uint64_t column = 0;
             column < resolved.outChannels; ++column) {
            inputs.config.biases[column] = loadedBiases[column];
        }
    }
    if (decision.inputFire) {
        inputs.inputValid = true;
        for (uint64_t row = 0; row < SauRows; ++row) {
            inputs.activations[row] = signedInt8(
                head.payload.activations[row]);
        }
        for (uint64_t column = 0;
             column < resolved.outChannels; ++column) {
            inputs.weights[column] =
                activeBEntry(head.tag.kIndex).weights[column];
        }
    }
    return inputs;
}

uint64_t
StreamingConvPipelineModel::outputIndex(
    const StreamingSpatialCoordinate &coordinate,
    uint64_t outputChannel) const
{
    if (!coordinate.valid || outputChannel >= resolved.outChannels) {
        throw std::out_of_range(
            "invalid streaming pipeline output coordinate");
    }
    uint64_t index = checkedMultiply(
        coordinate.n, resolved.outChannels,
        "streaming NCHW output index");
    index = checkedAdd(
        index, outputChannel, "streaming NCHW output index");
    index = checkedMultiply(
        index, resolved.im2col.outH,
        "streaming NCHW output index");
    index = checkedAdd(
        index, coordinate.oh, "streaming NCHW output index");
    index = checkedMultiply(
        index, resolved.im2col.outW,
        "streaming NCHW output index");
    return checkedAdd(
        index, coordinate.ow, "streaming NCHW output index");
}

void
StreamingConvPipelineModel::completeActiveTile()
{
    if (!activeEntry ||
        tileOutputRows != activeEntry->payload.validRows ||
        acceptedK != dimensions.k) {
        throw std::logic_error(
            "streaming tile completed before all work retired");
    }
    counters.tilesCompleted = checkedAdd(
        counters.tilesCompleted, 1,
        "streaming completed tile count");
    activeEntry.reset();
    acceptedK = 0;
    tileOutputRows = 0;
}

void
StreamingConvPipelineModel::checkInvariants() const
{
    if (fifoCount > StreamingFifoDepth ||
        fifoReadPointer >= StreamingFifoDepth ||
        fifoWritePointer >= StreamingFifoDepth) {
        throw std::logic_error("streaming FIFO state exceeds bounds");
    }
    const uint64_t pointerDistance =
        (fifoWritePointer + StreamingFifoDepth - fifoReadPointer) %
        StreamingFifoDepth;
    if (pointerDistance != fifoCount % StreamingFifoDepth) {
        throw std::logic_error("streaming FIFO pointers/count diverged");
    }
    if (counters.fifoPushes < counters.fifoPops ||
        counters.fifoPushes - counters.fifoPops != fifoCount ||
        counters.fifoPushes != producer.stats().outputVectors ||
        counters.fifoPops != counters.peInputCycles) {
        throw std::logic_error("streaming FIFO/vector conservation failed");
    }
    if (acceptedK > dimensions.k ||
        counters.tilesCompleted > counters.tilesLaunched ||
        counters.tilesLaunched > dimensions.expectedTiles ||
        counters.outputElements > dimensions.expectedOutputs) {
        throw std::logic_error("streaming pipeline count exceeds bounds");
    }
    const uint64_t activeAccepted = activeEntry ? acceptedK : 0;
    const uint64_t expectedPops = checkedAdd(
        checkedMultiply(
            counters.tilesCompleted, dimensions.k,
            "streaming accepted input conservation"),
        activeAccepted, "streaming accepted input conservation");
    if (counters.fifoPops != expectedPops ||
        counters.peLaunches != counters.tilesLaunched) {
        throw std::logic_error(
            "streaming consumer input/tile conservation failed");
    }
    const bool active =
        consumerState != StreamingConsumerState::Idle;
    if (active != activeEntry.has_value()) {
        throw std::logic_error(
            "streaming consumer/active metadata state diverged");
    }
    if (activeEntry && activeEntry->tag.tileIndex != activeTile) {
        throw std::logic_error("streaming active tile tag diverged");
    }
    const bool instructionIssued =
        consumerState == StreamingConsumerState::AcceptK ||
        consumerState == StreamingConsumerState::WaitResult ||
        consumerState == StreamingConsumerState::DrainOutput;
    const uint64_t expectedLaunched = counters.tilesCompleted +
        static_cast<uint64_t>(instructionIssued);
    if (counters.tilesLaunched != expectedLaunched) {
        throw std::logic_error(
            "streaming consumer launch/completion state diverged");
    }
    if (sauModel.protocol() != InputProtocol) {
        throw std::logic_error("streaming SA protocol changed");
    }
    const bool externalARead = std::any_of(
        aInFlight.valid.begin(), aInFlight.valid.end(),
        [](bool valid) { return valid; });
    if (externalARead != producer.hasPendingSharedRead()) {
        throw std::logic_error(
            "streaming external/model A in-flight state diverged");
    }
    if (counters.bBufferConsumedVectors != counters.fifoPops ||
        counters.bBufferHitVectors != counters.bBufferConsumedVectors ||
        counters.spadReadResponsesB > counters.spadReadGrantsB ||
        counters.spadReadGrantsB > counters.spadReadRequestsB ||
        counters.spadReadResponsesA > counters.spadReadGrantsA ||
        counters.spadReadGrantsA > counters.spadReadRequestsA ||
        counters.spadReadResponsesC > counters.spadReadGrantsC ||
        counters.spadReadGrantsC > counters.spadReadRequestsC ||
        counters.spadWriteGrantsD > counters.spadWriteRequestsD) {
        throw std::logic_error(
            "streaming shared request/consume conservation failed");
    }
    uint64_t bankReads = 0;
    uint64_t bankWrites = 0;
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        bankReads = checkedAdd(
            bankReads, counters.perBankReadCycles[bank],
            "per-bank read conservation");
        bankWrites = checkedAdd(
            bankWrites, counters.perBankWriteCycles[bank],
            "per-bank write conservation");
        if (counters.perBankReadCycles[bank] > cycleNumber + 1 ||
            counters.perBankWriteCycles[bank] > cycleNumber + 1 ||
            counters.perBankReadWriteConflicts[bank] > cycleNumber + 1) {
            throw std::logic_error(
                "per-bank cycle count exceeds elapsed cycles");
        }
    }
    const uint64_t totalReadGrants = checkedAdd(
        checkedAdd(
            counters.spadReadGrantsA, counters.spadReadGrantsB,
            "shared read-grant conservation"),
        counters.spadReadGrantsC, "shared read-grant conservation");
    if (bankReads != totalReadGrants ||
        bankWrites != counters.spadWriteGrantsD ||
        counters.bBufferOccupancySamples != cycleNumber + 1 ||
        counters.bBufferPeakOccupancy >
            checkedMultiply(
                dimensions.sharedSpad.bBufferDepth, 2,
                "combined B occupancy capacity")) {
        throw std::logic_error(
            "streaming per-bank/occupancy conservation failed");
    }
    if (dQueue.size() > dimensions.sharedSpad.dPendingRows ||
        counters.dPendingPeak > dimensions.sharedSpad.dPendingRows) {
        throw std::logic_error("D pending queue exceeds configured depth");
    }
    for (const auto &entry : dQueue) {
        if (!entry.coordinate.valid ||
            entry.validMask != expectedBMask() ||
            entry.pendingMask == 0 ||
            (entry.pendingMask & ~entry.validMask) != 0) {
            throw std::logic_error("D pending entry metadata is invalid");
        }
    }
    if (activeBBuffer >= bBuffers.size()) {
        throw std::logic_error("active B buffer index exceeds bounds");
    }
    for (uint64_t buffer = 0; buffer < bBuffers.size(); ++buffer) {
        const auto &value = bBuffers[buffer];
        if (!value.valid) {
            continue;
        }
        if (value.chunkLength == 0 ||
            value.chunkLength > dimensions.sharedSpad.bBufferDepth ||
            value.chunkBaseK + value.chunkLength > dimensions.k ||
            value.entries.size() !=
                dimensions.sharedSpad.bBufferDepth) {
            throw std::logic_error("B buffer chunk metadata is invalid");
        }
        for (uint64_t slot = 0; slot < value.chunkLength; ++slot) {
            const auto &entry =
                value.entries[static_cast<std::size_t>(slot)];
            if (!entry.valid || entry.weightTile != value.weightTile ||
                entry.globalK != value.chunkBaseK + slot ||
                (entry.readyMask & ~expectedBMask()) != 0) {
                throw std::logic_error("B buffer entry metadata is invalid");
            }
        }
    }
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        const bool bOwner = bInFlight[bank].valid;
        const bool aOwner = aInFlight.valid[bank];
        const bool cOwner = cInFlight[bank].valid;
        const uint64_t owners =
            static_cast<uint64_t>(aOwner) +
            static_cast<uint64_t>(bOwner) +
            static_cast<uint64_t>(cOwner);
        if (owners > 1) {
            throw std::logic_error(
                "scratchpad bank has multiple read owners");
        }
        if (spadInFlight.valid[bank] !=
            (aOwner || bOwner || cOwner)) {
            throw std::logic_error(
                "scratchpad in-flight request has no matching owner");
        }
    }
    if (biasesReady()) {
        const uint64_t expectedCReads =
            checkedMultiply(resolved.outChannels, 2, "expected C reads");
        if (counters.spadReadResponsesC != expectedCReads) {
            throw std::logic_error(
                "C ready without all bias-byte responses");
        }
    }
    if (activeEntry && consumerState ==
            StreamingConsumerState::AcceptK &&
        nextExpectedK != acceptedK) {
        throw std::logic_error(
            "B nextExpectedK diverged from consumer acceptedK");
    }
}

StreamingConvPipelineCycle
StreamingConvPipelineModel::tick()
{
    if (drainedAt) {
        throw std::logic_error("cannot tick a drained streaming pipeline");
    }
    const StreamingConsumerState oldConsumerState = consumerState;
    const uint64_t oldAcceptedK = acceptedK;
    const uint64_t oldFifoCount = fifoCount;
    const bool cWasReady = biasesReady();
    const uint64_t oldBReadyEntries = bReadyEntryCount(bBuffers);
    BReadTag oldBResponseTag;
    CReadTag oldCResponseTag;
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        if (bInFlight[bank].valid) {
            if (oldBResponseTag.valid &&
                (oldBResponseTag.buffer != bInFlight[bank].buffer ||
                 oldBResponseTag.slot != bInFlight[bank].slot ||
                 oldBResponseTag.globalK != bInFlight[bank].globalK)) {
                throw std::logic_error(
                    "one cycle contains multiple B response identities");
            }
            oldBResponseTag = bInFlight[bank];
        }
        if (cInFlight[bank].valid) {
            if (oldCResponseTag.valid &&
                oldCResponseTag.byte != cInFlight[bank].byte) {
                throw std::logic_error(
                    "one cycle contains multiple C response bytes");
            }
            oldCResponseTag = cInFlight[bank];
        }
    }
    refreshActiveB();
    auto workingB = bBuffers;
    auto workingD = dQueue;
    const SramResponse spadResponse =
        sharedScratchpad.combinationalResponse(spadInFlight);
    SramResponse aResponse;
    SramResponse bResponse;
    SramResponse cResponse;
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        if (aInFlight.valid[bank]) {
            aResponse.valid[bank] = spadResponse.valid[bank];
            aResponse.data[bank] = spadResponse.data[bank];
        }
        if (bInFlight[bank].valid) {
            bResponse.valid[bank] = spadResponse.valid[bank];
            bResponse.data[bank] = spadResponse.data[bank];
        }
        if (cInFlight[bank].valid) {
            cResponse.valid[bank] = spadResponse.valid[bank];
            cResponse.data[bank] = spadResponse.data[bank];
        }
    }
    applyCResponses(cResponse);
    applyBResponses(workingB, bResponse);

    const bool fifoValid = oldFifoCount != 0;
    const StreamingFifoEntry head = fifoValid ?
        fifo[fifoReadPointer] : StreamingFifoEntry{};
    bool bReadyForConsumer = true;
    if (fifoValid) {
        if (oldConsumerState == StreamingConsumerState::Idle) {
            bReadyForConsumer = bLaunchReady(head);
        } else if (oldConsumerState ==
                   StreamingConsumerState::AcceptK) {
            bReadyForConsumer = bInputReady(head);
        }
    }
    const bool consumerFifoValid =
        cWasReady && fifoValid && bReadyForConsumer;
    const auto consumerDecision = decideStreamingConsumer(
        oldConsumerState, consumerFifoValid, head.tag,
        activeTile, oldAcceptedK, dimensions.k);
    const bool fifoPop = consumerDecision.inputFire;
    const bool fifoPushReady =
        oldFifoCount < StreamingFifoDepth || fifoPop;
    if (fifoValid &&
        oldConsumerState == StreamingConsumerState::AcceptK &&
        !bReadyForConsumer) {
        counters.bBufferEmptyCycles = checkedAdd(
            counters.bBufferEmptyCycles, 1,
            "B buffer empty cycle count");
    }

    const BRequestContext bContext =
        cWasReady ? buildBRequest(workingB) : BRequestContext{};
    const CRequestContext cContext =
        cWasReady ? CRequestContext{} : buildCRequest();
    const SramRequest dRequest =
        cWasReady ? buildDRequest() : SramRequest{};
    SramRequest bGrant;
    SramRequest cGrant;
    SramRequest dGrant;
    SramRequest combinedGrant;

    PipelinedIm2ColCycle producerCycle;
    if (!cWasReady) {
        producerCycle.cycle = cycleNumber;
        const auto arbitration = arbitrateSharedSpad(
            {}, {}, cContext.request, {});
        cGrant = arbitration.cGrant;
        combinedGrant = arbitration.readGrant;
    } else if (!producer.hasDrained()) {
        producerCycle = producer.tickShared(
            fifoPushReady,
            aResponse,
            [&](const SramRequest &aRequest) {
                const auto arbitration = arbitrateSharedSpad(
                    aRequest, bContext.request, {}, dRequest);
                bGrant = arbitration.bGrant;
                dGrant = arbitration.dGrant;
                combinedGrant = arbitration.readGrant;
                return arbitration.aGrant;
            });
        aInFlight = producerCycle.grant;
    } else {
        if (std::any_of(
                aInFlight.valid.begin(), aInFlight.valid.end(),
                [](bool valid) { return valid; })) {
            throw std::logic_error(
                "streaming producer drained with A read in flight");
        }
        producerCycle.cycle = cycleNumber;
        producerCycle.s0Ready = true;
        producerCycle.s1Ready = true;
        producerCycle.s2Ready = fifoPushReady;
        producerCycle.producerExhausted = true;
        producerCycle.drained = true;
        const auto arbitration = arbitrateSharedSpad(
            {}, bContext.request, {}, dRequest);
        bGrant = arbitration.bGrant;
        dGrant = arbitration.dGrant;
        combinedGrant = arbitration.readGrant;
    }
    counters.spadReadRequestsC = checkedAdd(
        counters.spadReadRequestsC, requestCount(cContext.request),
        "C read request count");
    counters.spadReadGrantsC = checkedAdd(
        counters.spadReadGrantsC, requestCount(cGrant),
        "C read grant count");
    counters.spadReadRequestsA = checkedAdd(
        counters.spadReadRequestsA, requestCount(producerCycle.request),
        "A read request count");
    counters.spadReadGrantsA = checkedAdd(
        counters.spadReadGrantsA, requestCount(producerCycle.grant),
        "A read grant count");
    counters.spadReadResponsesA = checkedAdd(
        counters.spadReadResponsesA, responseCount(producerCycle.response),
        "A read response count");
    counters.spadReadRequestsB = checkedAdd(
        counters.spadReadRequestsB, requestCount(bContext.request),
        "B read request count");
    counters.spadReadGrantsB = checkedAdd(
        counters.spadReadGrantsB, requestCount(bGrant),
        "B read grant count");
    if (requestCount(bGrant) < requestCount(bContext.request)) {
        counters.bPrefetchStallCycles = checkedAdd(
            counters.bPrefetchStallCycles, 1,
            "B prefetch stall cycle count");
    }
    counters.spadWriteRequestsD = checkedAdd(
        counters.spadWriteRequestsD, requestCount(dRequest),
        "D write request count");
    counters.spadWriteGrantsD = checkedAdd(
        counters.spadWriteGrantsD, requestCount(dGrant),
        "D write grant count");
    if (anySpadRequest(dRequest) &&
        requestCount(dGrant) < requestCount(dRequest)) {
        counters.dWriteStallCycles = checkedAdd(
            counters.dWriteStallCycles, 1,
            "D write stall cycle count");
    }
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        const bool readGrant =
            producerCycle.grant.valid[bank] ||
            bGrant.valid[bank] || cGrant.valid[bank];
        if (readGrant) {
            counters.perBankReadCycles[bank] = checkedAdd(
                counters.perBankReadCycles[bank], 1,
                "per-bank read cycle count");
        }
        if (dGrant.valid[bank]) {
            counters.perBankWriteCycles[bank] = checkedAdd(
                counters.perBankWriteCycles[bank], 1,
                "per-bank write cycle count");
        }
        const bool competingRead =
            producerCycle.request.valid[bank] ||
            bContext.request.valid[bank] ||
            cContext.request.valid[bank];
        if (dRequest.valid[bank] && competingRead) {
            counters.perBankReadWriteConflicts[bank] = checkedAdd(
                counters.perBankReadWriteConflicts[bank], 1,
                "per-bank read/write conflict count");
        }
    }
    spadInFlight = combinedGrant;
    installCInFlight(cContext, cGrant);
    installBInFlight(bContext, bGrant);
    uint16_t dGrantMask = 0;
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        if (dGrant.valid[bank]) {
            dGrantMask |= uint16_t{1} << bank;
        }
    }
    const auto dDecision = decideDPendingQueue(
        dQueue.size(), dimensions.sharedSpad.dPendingRows,
        dQueue.empty() ? 0 : dQueue.front().pendingMask,
        dGrantMask, outputReady(cycleNumber, readyConfig));
    const bool dHeadWillRetire = dDecision.headWillRetire;
    const bool dDequeue = dHeadWillRetire;
    applyDWrites(workingD, dGrant);
    const bool outputGrant = dDecision.outputGrant;

    const bool fifoPush = producerCycle.s2Fire;
    const auto fifoDecision = decideElasticFifo(
        oldFifoCount, producerCycle.s2Valid, fifoPop);
    if (fifoDecision.pushReady != fifoPushReady ||
        fifoDecision.push != fifoPush ||
        fifoDecision.pop != fifoPop) {
        throw std::logic_error(
            "streaming FIFO decision diverged from producer/consumer");
    }

    StreamingConvPipelineCycle observation;
    observation.cycle = cycleNumber;
    observation.consumerState = oldConsumerState;
    observation.acceptedK = oldAcceptedK;
    observation.activeTile = activeTile;
    observation.fifoCount = oldFifoCount;
    observation.fifoReadPointer = fifoReadPointer;
    observation.fifoWritePointer = fifoWritePointer;
    observation.fifoHeadValid = fifoValid;
    observation.fifoHead = head;
    observation.fifoPushReady = fifoPushReady;
    observation.fifoPush = fifoPush;
    observation.fifoPop = fifoPop;
    observation.bRequest = bContext.request;
    observation.bGrant = bGrant;
    observation.bResponse = bResponse;
    observation.bRequestBuffer =
        bContext.valid ? bContext.buffer : 0;
    observation.bRequestSlot = bContext.valid ? bContext.slot : 0;
    observation.bRequestK = bContext.valid ? bContext.globalK : 0;
    observation.bResponseBuffer =
        oldBResponseTag.valid ? oldBResponseTag.buffer : 0;
    observation.bResponseSlot =
        oldBResponseTag.valid ? oldBResponseTag.slot : 0;
    observation.bResponseK =
        oldBResponseTag.valid ? oldBResponseTag.globalK : 0;
    observation.cRequest = cContext.request;
    observation.cGrant = cGrant;
    observation.cResponse = cResponse;
    observation.cRequestByte =
        cContext.request.valid[0] ? cContext.bytes[0] : 0;
    observation.cResponseByte =
        oldCResponseTag.valid ? oldCResponseTag.byte : 0;
    observation.dRequest = dRequest;
    observation.dGrant = dGrant;
    observation.dQueueOccupancy = dQueue.size();
    observation.dHeadPendingMask =
        dQueue.empty() ? 0 : dQueue.front().pendingMask;
    observation.dHeadWillRetire = dHeadWillRetire;
    observation.dDequeue = dDequeue;
    observation.bEntryHit = fifoValid &&
        oldConsumerState == StreamingConsumerState::AcceptK &&
        bReadyForConsumer;
    observation.bReuseHit = observation.bEntryHit &&
        fullBResident && activeTile != 0;
    observation.activeBBuffer = activeBBuffer;
    observation.nextExpectedK = nextExpectedK;
    observation.bReadyEntries = oldBReadyEntries;
    observation.consumer = consumerDecision;
    observation.producer = producerCycle;

    if (consumerDecision.beginLaunch) {
        if (head.tag.tileIndex != counters.tilesLaunched) {
            throw std::logic_error(
                "streaming launch tile sequence is not contiguous");
        }
        activeEntry = head;
        activeTile = head.tag.tileIndex;
        acceptedK = 0;
        tileOutputRows = 0;
    }
    observation.sauInputs = buildSauInputs(
        consumerDecision, head, outputGrant);
    observation.sau = sauModel.tick(observation.sauInputs);
    observation.outputCollected = observation.sau.rowScoreValid;
    observation.dEnqueue = observation.sau.rowScoreValid;
    enqueueDOutput(workingD, observation.sau);
    bBuffers = std::move(workingB);
    dQueue = std::move(workingD);
    counters.dPendingPeak = std::max(
        counters.dPendingPeak,
        static_cast<uint64_t>(dQueue.size()));
    counters.bBufferOccupancySamples = checkedAdd(
        counters.bBufferOccupancySamples, 1,
        "B buffer occupancy sample count");
    counters.bBufferOccupancySum = checkedAdd(
        counters.bBufferOccupancySum, oldBReadyEntries,
        "B buffer occupancy sum");
    counters.bBufferPeakOccupancy = std::max(
        counters.bBufferPeakOccupancy, oldBReadyEntries);
    if (consumerDecision.inputFire) {
        consumeBInput(head.tag.kIndex);
    }

    counters.fifoOccupancySamples = checkedAdd(
        counters.fifoOccupancySamples, 1,
        "streaming FIFO occupancy sample count");
    counters.fifoOccupancySum = checkedAdd(
        counters.fifoOccupancySum, oldFifoCount,
        "streaming FIFO occupancy sum");
    counters.fifoPeakOccupancy = std::max(
        counters.fifoPeakOccupancy, oldFifoCount);
    if (oldFifoCount == StreamingFifoDepth) {
        counters.fifoFullCycles = checkedAdd(
            counters.fifoFullCycles, 1,
            "streaming FIFO full cycle count");
    }
    if (oldConsumerState == StreamingConsumerState::AcceptK &&
        oldAcceptedK < dimensions.k && !fifoPop) {
        counters.peInputBubbleCycles = checkedAdd(
            counters.peInputBubbleCycles, 1,
            "streaming PE input bubble count");
    }
    if (oldConsumerState == StreamingConsumerState::WaitResult ||
        oldConsumerState == StreamingConsumerState::DrainOutput) {
        counters.peBusyNotAcceptingCycles = checkedAdd(
            counters.peBusyNotAcceptingCycles, 1,
            "streaming PE busy-not-accepting count");
    }

    if (fifoPop) {
        fifoReadPointer =
            (fifoReadPointer + 1) % StreamingFifoDepth;
        counters.fifoPops = checkedAdd(
            counters.fifoPops, 1, "streaming FIFO pop count");
        counters.peInputCycles = checkedAdd(
            counters.peInputCycles, 1,
            "streaming PE input cycle count");
    }
    if (fifoPush) {
        fifo[fifoWritePointer] = producerCycle.output;
        fifoWritePointer =
            (fifoWritePointer + 1) % StreamingFifoDepth;
        counters.fifoPushes = checkedAdd(
            counters.fifoPushes, 1, "streaming FIFO push count");
    }
    fifoCount = fifoDecision.nextCount;
    counters.tilesGenerated =
        producer.stats().inputVectors / dimensions.k;

    switch (oldConsumerState) {
      case StreamingConsumerState::Idle:
        if (consumerDecision.beginLaunch) {
            consumerState = StreamingConsumerState::Launch;
        }
        break;
      case StreamingConsumerState::Launch:
        if (!consumerDecision.launch) {
            throw std::logic_error(
                "streaming LAUNCH cycle did not issue instruction");
        }
        counters.peLaunches = checkedAdd(
            counters.peLaunches, 1, "streaming PE launch count");
        counters.tilesLaunched = checkedAdd(
            counters.tilesLaunched, 1,
            "streaming launched tile count");
        consumerState = StreamingConsumerState::AcceptK;
        break;
      case StreamingConsumerState::AcceptK:
        if (fifoPop) {
            if (!activeEntry) {
                throw std::logic_error(
                    "streaming input fire requires active metadata");
            }
            validateSameTileMetadata(
                activeEntry->tag, activeEntry->payload,
                head.tag, head.payload);
            acceptedK = checkedAdd(
                acceptedK, 1, "streaming accepted K count");
            if (acceptedK == dimensions.k) {
                consumerState = StreamingConsumerState::WaitResult;
            }
        }
        break;
      case StreamingConsumerState::WaitResult:
        if (observation.sau.storageReady) {
            consumerState = StreamingConsumerState::DrainOutput;
        }
        break;
      case StreamingConsumerState::DrainOutput:
        if (observation.sau.calFinish) {
            completeActiveTile();
            consumerState = StreamingConsumerState::Idle;
        }
        break;
    }

    checkInvariants();
    const bool scratchpadIdle = !anySpadRequest(spadInFlight) &&
        !producer.hasPendingSharedRead() &&
        std::none_of(
            cInFlight.begin(), cInFlight.end(),
            [](const CReadTag &tag) { return tag.valid; }) &&
        std::none_of(
            bInFlight.begin(), bInFlight.end(),
            [](const BReadTag &tag) { return tag.valid; });
    const bool drained = producer.hasDrained() && fifoCount == 0 &&
        consumerState == StreamingConsumerState::Idle &&
        sauModel.state() == SauEngineState::Idle && scratchpadIdle &&
        biasesReady() && dQueue.empty() &&
        !buildBRequest(bBuffers).valid;
    if (drained) {
        validateDrainedConservation({
            dimensions.im2col.expectedVectors,
            producer.stats().inputVectors,
            producer.stats().outputVectors,
            counters.fifoPushes,
            counters.fifoPops,
            counters.peInputCycles,
            dimensions.expectedTiles,
            counters.tilesGenerated,
            counters.tilesLaunched,
            counters.tilesCompleted,
        });
        if (counters.outputElements != dimensions.expectedOutputs) {
            throw std::logic_error(
                "streaming pipeline drained with incomplete outputs");
        }
        if (counters.bBufferConsumedVectors !=
                dimensions.im2col.expectedVectors ||
            counters.bBufferHitVectors !=
                dimensions.im2col.expectedVectors) {
            throw std::logic_error(
                "streaming pipeline drained with incomplete B consumption");
        }
        if (checkedMultiply(
                counters.outputRows, resolved.outChannels,
                "streaming drained output rows") !=
            dimensions.expectedOutputs) {
            throw std::logic_error(
                "streaming pipeline drained with incomplete output rows");
        }
        rebuildOutputsFromD();
        drainedAt = cycleNumber;
        observation.drained = true;
    }
    cycleNumber = checkedAdd(
        cycleNumber, 1, "streaming pipeline cycle");
    return observation;
}

} // namespace gem5::sau_n
