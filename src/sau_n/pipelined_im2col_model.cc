#include "sau_n/pipelined_im2col_model.hh"

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>

#include "sau_n/im2col_address.hh"

namespace gem5::sau_n
{
namespace
{

bool
anyRequest(const SramRequest &request)
{
    return std::any_of(
        request.valid.begin(), request.valid.end(),
        [](bool valid) { return valid; });
}

uint64_t
minimumReadRounds(const StreamingS1Payload &payload)
{
    uint64_t result = 0;
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        std::array<uint64_t, SauRows> rows{};
        uint64_t rowCount = 0;
        for (const auto &request : payload.requests) {
            if (request.source != StreamingLaneSource::ScratchpadRead ||
                request.bank != bank) {
                continue;
            }
            const auto begin = rows.begin();
            const auto end = begin + static_cast<std::ptrdiff_t>(rowCount);
            if (std::find(begin, end, request.row) == end) {
                rows[rowCount++] = request.row;
            }
        }
        result = std::max(result, rowCount);
    }
    return std::max(uint64_t{1}, result);
}

} // anonymous namespace

PipelinedIm2ColModel::PipelinedIm2ColModel(
    const PipelineResolvedConfig &config)
    : resolved(config), dimensions(validateStreamingConfig(resolved)),
      standaloneScratchpad(std::make_unique<BankedScratchpad>())
{
    standaloneScratchpad->preload(resolved.im2col);
}

PipelinedIm2ColModel::PipelinedIm2ColModel(
    const PipelineResolvedConfig &config,
    PipelinedIm2ColMemoryMode memoryMode)
    : resolved(config), dimensions(validateStreamingConfig(resolved)),
      accessMode(memoryMode)
{
    if (accessMode != PipelinedIm2ColMemoryMode::SharedOneCycle) {
        throw std::invalid_argument(
            "explicit PipelinedIm2Col memory mode must be SharedOneCycle");
    }
}

PipelinedIm2ColModel::PipelinedIm2ColModel(
    const PipelineResolvedConfig &config,
    const BankedScratchpad &preloadedScratchpad)
    : resolved(config), dimensions(validateStreamingConfig(resolved)),
      standaloneScratchpad(
          std::make_unique<BankedScratchpad>(preloadedScratchpad))
{
}

bool
PipelinedIm2ColModel::hasPendingSharedRead() const
{
    return anyRequest(sharedInFlight);
}

StreamingS0Payload
PipelinedIm2ColModel::buildS0Payload(const Iterators &iterators) const
{
    StreamingS0Payload payload;
    payload.tag.tileIndex = iterators.tileIndex;
    payload.tag.validColumns = resolved.outChannels;
    payload.tag.c = iterators.c;
    payload.tag.kh = iterators.kh;
    payload.tag.kw = iterators.kw;
    payload.tag.kIndex = iterators.c * 9 + iterators.kh * 3 + iterators.kw;
    payload.tag.tileFirst = payload.tag.kIndex == 0;
    payload.tag.tileLast = payload.tag.kIndex + 1 == dimensions.k;
    validateVectorTag(payload.tag, dimensions.k);

    const ChwAddressMapper mapper(resolved.im2col);
    for (uint64_t lane = 0; lane < SauRows; ++lane) {
        uint64_t localH = 0;
        uint64_t localW = lane;
        uint64_t outH = iterators.oh;
        uint64_t outW = iterators.owBase + lane;
        if (resolved.im2col.w <= BlockSize) {
            localH = lane / resolved.im2col.w;
            localW = lane % resolved.im2col.w;
            outH = iterators.oh + localH;
            outW = localW;
        }

        const bool shapeValid = resolved.im2col.w > BlockSize ||
            localH < dimensions.im2col.rowsPerWord;
        const bool outputValid = shapeValid &&
            outH < resolved.im2col.outH && outW < resolved.im2col.outW;
        if (!outputValid) {
            continue;
        }

        auto &request = payload.lanes[lane];
        request.coordinate = {true, iterators.n, outH, outW};
        const uint64_t paddedH = checkedAdd(
            checkedMultiply(
                outH, resolved.im2col.strideH, "streaming padded H"),
            checkedMultiply(
                iterators.kh, resolved.im2col.dilationH,
                "streaming padded H"),
            "streaming padded H");
        const uint64_t paddedW = checkedAdd(
            checkedMultiply(
                outW, resolved.im2col.strideW, "streaming padded W"),
            checkedMultiply(
                iterators.kw, resolved.im2col.dilationW,
                "streaming padded W"),
            "streaming padded W");
        const bool padding = paddedH < resolved.im2col.padTop ||
            paddedW < resolved.im2col.padLeft ||
            paddedH - resolved.im2col.padTop >= resolved.im2col.h ||
            paddedW - resolved.im2col.padLeft >= resolved.im2col.w;
        if (padding) {
            request.source = StreamingLaneSource::PaddingZero;
            continue;
        }

        request.source = StreamingLaneSource::ScratchpadRead;
        request.inputH = paddedH - resolved.im2col.padTop;
        request.inputW = paddedW - resolved.im2col.padLeft;
        const auto address = mapper.locate(
            iterators.n, iterators.c, request.inputH, request.inputW);
        request.bank = address.bank;
        request.row = address.row;
    }
    return payload;
}

StreamingS1Payload
PipelinedIm2ColModel::beginS1(const StreamingS0Payload &payload) const
{
    StreamingS1Payload result;
    result.tag = payload.tag;
    result.requests = payload.lanes;
    for (uint64_t lane = 0; lane < SauRows; ++lane) {
        const auto &request = result.requests[lane];
        if (request.coordinate.valid) {
            result.rawSpatialMask |= static_cast<uint16_t>(1U << lane);
        }
        result.laneDone[lane] =
            request.source != StreamingLaneSource::ScratchpadRead;
    }
    return result;
}

SramRequest
PipelinedIm2ColModel::arbitrateS1(
    const StreamingS1Payload &payload) const
{
    SramRequest result;
    for (uint64_t lane = 0; lane < SauRows; ++lane) {
        const auto &candidate = payload.requests[lane];
        if (candidate.source != StreamingLaneSource::ScratchpadRead ||
            payload.laneDone[lane]) {
            continue;
        }
        if (candidate.bank >= SpBanks || candidate.row >= SpBankEntries) {
            throw std::out_of_range(
                "streaming lane request exceeds scratchpad bounds");
        }
        if (!result.valid[candidate.bank]) {
            result.valid[candidate.bank] = true;
            result.address[candidate.bank] =
                static_cast<uint16_t>(candidate.row);
        }
    }
    return result;
}

StreamingS1Payload
PipelinedIm2ColModel::collectS1(
    const StreamingS1Payload &payload,
    const SramRequest &request,
    const SramResponse &response) const
{
    StreamingS1Payload result = payload;
    const bool hasRequest = anyRequest(request);
    const bool firstZeroRequestRound =
        !hasRequest && payload.completedReadRounds == 0 &&
        s1Complete(payload);
    if (hasRequest || firstZeroRequestRound) {
        result.completedReadRounds = checkedAdd(
            result.completedReadRounds, 1,
            "streaming completed read round count");
    }
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        if (!response.valid[bank]) {
            continue;
        }
        for (uint64_t lane = 0; lane < SauRows; ++lane) {
            const auto &candidate = payload.requests[lane];
            if (candidate.source == StreamingLaneSource::ScratchpadRead &&
                !payload.laneDone[lane] && candidate.bank == bank &&
                candidate.row == request.address[bank]) {
                result.activations[lane] = response.data[bank];
                result.laneDone[lane] = true;
            }
        }
    }
    return result;
}

bool
PipelinedIm2ColModel::s1Complete(
    const StreamingS1Payload &payload) const
{
    return std::all_of(
        payload.laneDone.begin(), payload.laneDone.end(),
        [](bool done) { return done; });
}

StreamingS2Payload
PipelinedIm2ColModel::finishS2(
    const StreamingS1Payload &payload) const
{
    if (!s1Complete(payload)) {
        throw std::logic_error("incomplete S1 payload cannot enter S2");
    }
    StreamingS2Payload result;
    result.tag = payload.tag;
    result.raw.activations = payload.activations;
    result.raw.spatialMask = payload.rawSpatialMask;
    for (uint64_t lane = 0; lane < SauRows; ++lane) {
        result.raw.coordinates[lane] = payload.requests[lane].coordinate;
    }
    result.compacted = compactSpatialPayload(result.raw);
    result.readRounds = payload.completedReadRounds;
    return result;
}

bool
PipelinedIm2ColModel::advanceIterators(Iterators &iterators) const
{
    if (iterators.kw + 1 < resolved.im2col.kernelW) {
        ++iterators.kw;
        return false;
    }
    iterators.kw = 0;
    if (iterators.kh + 1 < resolved.im2col.kernelH) {
        ++iterators.kh;
        return false;
    }
    iterators.kh = 0;
    if (iterators.c + 1 < resolved.im2col.c) {
        ++iterators.c;
        return false;
    }
    iterators.c = 0;
    iterators.tileIndex = checkedAdd(
        iterators.tileIndex, 1, "streaming tile iterator");
    if (resolved.im2col.w > BlockSize &&
        iterators.owBase + BlockSize < resolved.im2col.outW) {
        iterators.owBase += BlockSize;
        return false;
    }
    iterators.owBase = 0;
    if (resolved.im2col.w <= BlockSize &&
        iterators.oh + dimensions.im2col.rowsPerWord <
            resolved.im2col.outH) {
        iterators.oh += dimensions.im2col.rowsPerWord;
        return false;
    }
    if (resolved.im2col.w > BlockSize &&
        iterators.oh + 1 < resolved.im2col.outH) {
        ++iterators.oh;
        return false;
    }
    iterators.oh = 0;
    if (iterators.n + 1 < resolved.im2col.n) {
        ++iterators.n;
        return false;
    }
    return true;
}

void
PipelinedIm2ColModel::noteGenerated(const StreamingS0Payload &payload)
{
    if (lastGeneratedTag) {
        validateVectorSequence(
            *lastGeneratedTag, payload.tag, dimensions.k);
    }
    lastGeneratedTag = payload.tag;
    if (lastGeneratedCycle) {
        counters.producerInputPairs = checkedAdd(
            counters.producerInputPairs, 1,
            "streaming producer input pair count");
        counters.producerInputGapCycles = checkedAdd(
            counters.producerInputGapCycles,
            cycleNumber - *lastGeneratedCycle,
            "streaming producer input gap cycles");
    }
    lastGeneratedCycle = cycleNumber;
    counters.inputVectors = checkedAdd(
        counters.inputVectors, 1, "streaming input vector count");
}

void
PipelinedIm2ColModel::noteRetiredS1(
    const StreamingS1Payload &payload)
{
    const uint64_t minimum = minimumReadRounds(payload);
    const bool invalidRounds =
        accessMode == PipelinedIm2ColMemoryMode::StandaloneCombinational ?
        payload.completedReadRounds != minimum :
        payload.completedReadRounds < minimum;
    if (invalidRounds) {
        throw std::logic_error(
            "S1 did not retire in the minimum single-port read rounds");
    }
    if (minimum > 1) {
        counters.bankConflictVectors = checkedAdd(
            counters.bankConflictVectors, 1,
            "streaming bank-conflict vector count");
        counters.bankConflictExtraRounds = checkedAdd(
            counters.bankConflictExtraRounds,
            minimum - 1,
            "streaming extra read round count");
    }
}

void
PipelinedIm2ColModel::notePushed(const StreamingS2Payload &payload)
{
    if (lastPushedTag) {
        validateVectorSequence(*lastPushedTag, payload.tag, dimensions.k);
    }
    if (lastPushedEntry &&
        lastPushedEntry->tag.tileIndex == payload.tag.tileIndex) {
        validateSameTileMetadata(
            lastPushedEntry->tag, lastPushedEntry->payload,
            payload.tag, payload.compacted);
    }
    if (!isCanonicalPrefixMask(payload.compacted.spatialMask)) {
        throw std::logic_error("S2 produced a non-prefix spatial mask");
    }
    if (payload.readRounds == 0) {
        throw std::logic_error("S2 payload has no completed read round");
    }
    if (counters.pipelineFillCycles == 0) {
        counters.pipelineFillCycles = checkedAdd(
            cycleNumber, 1, "streaming pipeline fill cycles");
    }
    if (lastPushedCycle) {
        const uint64_t gap = cycleNumber - *lastPushedCycle;
        counters.im2colOutputPairs = checkedAdd(
            counters.im2colOutputPairs, 1,
            "streaming Im2Col output pair count");
        counters.im2colOutputGapCycles = checkedAdd(
            counters.im2colOutputGapCycles, gap,
            "streaming Im2Col output gap cycles");
        if (lastPushedConflictFree && payload.readRounds == 1 &&
            !downstreamStallSinceLastPush) {
            counters.conflictFreeOutputPairs = checkedAdd(
                counters.conflictFreeOutputPairs, 1,
                "streaming conflict-free output pair count");
            counters.conflictFreeOutputGapCycles = checkedAdd(
                counters.conflictFreeOutputGapCycles, gap,
                "streaming conflict-free output gap cycles");
            counters.conflictFreeOutputMaxGap = std::max(
                counters.conflictFreeOutputMaxGap, gap);
        }
    }
    lastPushedCycle = cycleNumber;
    lastPushedConflictFree = payload.readRounds == 1;
    downstreamStallSinceLastPush = false;
    lastPushedTag = payload.tag;
    lastPushedEntry = StreamingFifoEntry{payload.tag, payload.compacted};
    counters.outputVectors = checkedAdd(
        counters.outputVectors, 1, "streaming output vector count");
    counters.compactedSpatialVectors = checkedAdd(
        counters.compactedSpatialVectors, 1,
        "streaming compacted vector count");
}

void
PipelinedIm2ColModel::checkS0Payload(
    const StreamingS0Payload &payload) const
{
    validateVectorTag(payload.tag, dimensions.k);
    for (const auto &lane : payload.lanes) {
        if ((lane.source == StreamingLaneSource::InvalidTail) !=
            !lane.coordinate.valid) {
            throw std::logic_error(
                "S0 lane source and spatial validity diverged");
        }
        if (lane.source != StreamingLaneSource::ScratchpadRead &&
            (lane.inputH != 0 || lane.inputW != 0 ||
             lane.bank != 0 || lane.row != 0)) {
            throw std::logic_error(
                "non-read S0 lane must use canonical address metadata");
        }
        if (lane.source == StreamingLaneSource::ScratchpadRead &&
            (lane.bank >= SpBanks || lane.row >= SpBankEntries)) {
            throw std::logic_error("S0 scratchpad address exceeds bounds");
        }
    }
}

void
PipelinedIm2ColModel::checkS1Payload(
    const StreamingS1Payload &payload) const
{
    validateVectorTag(payload.tag, dimensions.k);
    uint16_t expectedMask = 0;
    for (uint64_t lane = 0; lane < SauRows; ++lane) {
        const auto &request = payload.requests[lane];
        if (request.coordinate.valid) {
            expectedMask |= static_cast<uint16_t>(1U << lane);
        }
        if (request.source != StreamingLaneSource::ScratchpadRead &&
            !payload.laneDone[lane]) {
            throw std::logic_error("non-read S1 lane cannot remain pending");
        }
        if (request.source != StreamingLaneSource::ScratchpadRead &&
            payload.activations[lane] != 0) {
            throw std::logic_error(
                "non-read S1 lane must retain canonical zero data");
        }
        if (!payload.laneDone[lane] && payload.activations[lane] != 0) {
            throw std::logic_error(
                "pending S1 lane must retain canonical zero data");
        }
    }
    if (payload.rawSpatialMask != expectedMask || expectedMask == 0) {
        throw std::logic_error("S1 raw spatial mask is inconsistent");
    }
}

void
PipelinedIm2ColModel::checkS2Payload(
    const StreamingS2Payload &payload) const
{
    validateVectorTag(payload.tag, dimensions.k);
    if (!(compactSpatialPayload(payload.raw) == payload.compacted)) {
        throw std::logic_error(
            "S2 compacted payload diverged from its raw lanes");
    }
    if (payload.readRounds == 0) {
        throw std::logic_error("S2 read round count must be nonzero");
    }
}

void
PipelinedIm2ColModel::checkInvariants(const Registers &value) const
{
    if (counters.inputVectors > dimensions.im2col.expectedVectors ||
        counters.outputVectors > counters.inputVectors) {
        throw std::logic_error("streaming vector count exceeds bounds");
    }
    const uint64_t stages = static_cast<uint64_t>(value.s0Valid) +
        static_cast<uint64_t>(value.s1Valid) +
        static_cast<uint64_t>(value.s2Valid);
    if (counters.inputVectors - counters.outputVectors != stages) {
        throw std::logic_error("streaming stage vector conservation failed");
    }
    if (value.producerExhausted && value.s0Valid) {
        throw std::logic_error(
            "streaming producer exhausted while S0 remains valid");
    }
    if (value.s0Valid) {
        checkS0Payload(value.s0);
    }
    if (value.s1Valid) {
        checkS1Payload(value.s1);
    }
    if (value.s2Valid) {
        checkS2Payload(value.s2);
    }
    if (value.producerExhausted &&
        value.iterators.tileIndex != dimensions.expectedTiles) {
        throw std::logic_error(
            "streaming tile iterator did not reach expected tile count");
    }
    if (accessMode == PipelinedIm2ColMemoryMode::SharedOneCycle &&
        hasPendingSharedRead() && !value.s1Valid) {
        throw std::logic_error(
            "shared A read remains in flight without an S1 context");
    }
}

void
PipelinedIm2ColModel::validateSharedGrant(
    const SramRequest &request, const SramRequest &grant) const
{
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        if (grant.valid[bank] &&
            (!request.valid[bank] ||
             grant.address[bank] != request.address[bank])) {
            throw std::invalid_argument(
                "shared A grant is not a subset of the proposed request");
        }
    }
}

void
PipelinedIm2ColModel::validateSharedResponse(
    const SramResponse &response) const
{
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        if (response.valid[bank] != sharedInFlight.valid[bank]) {
            throw std::invalid_argument(
                "shared A response valid mask does not match prior grant");
        }
    }
}

PipelinedIm2ColCycle
PipelinedIm2ColModel::tick(bool fifoPushReady)
{
    if (accessMode !=
        PipelinedIm2ColMemoryMode::StandaloneCombinational) {
        throw std::logic_error(
            "shared PipelinedIm2Col model requires tickShared");
    }
    return tickImpl(fifoPushReady, nullptr, nullptr);
}

PipelinedIm2ColCycle
PipelinedIm2ColModel::tickShared(
    bool fifoPushReady,
    const SramResponse &previousResponse,
    const SharedAGrantFunction &grantFunction)
{
    if (accessMode != PipelinedIm2ColMemoryMode::SharedOneCycle) {
        throw std::logic_error(
            "standalone PipelinedIm2Col model requires tick");
    }
    if (!grantFunction) {
        throw std::invalid_argument(
            "shared A grant function must not be empty");
    }
    validateSharedResponse(previousResponse);
    return tickImpl(
        fifoPushReady, &previousResponse, &grantFunction);
}

PipelinedIm2ColCycle
PipelinedIm2ColModel::tickImpl(
    bool fifoPushReady,
    const SramResponse *previousResponse,
    const SharedAGrantFunction *grantFunction)
{
    if (drainedAt) {
        throw std::logic_error("pipelined Im2Col ticked after drained");
    }
    const Registers old = registers;
    const bool shared =
        accessMode == PipelinedIm2ColMemoryMode::SharedOneCycle;
    const SramRequest responseRequest = shared ?
        sharedInFlight :
        (old.s1Valid ? arbitrateS1(old.s1) : SramRequest{});
    const SramResponse response = shared ?
        *previousResponse :
        standaloneScratchpad->combinationalResponse(responseRequest);
    const StreamingS1Payload collected = old.s1Valid ?
        collectS1(old.s1, responseRequest, response) :
        StreamingS1Payload{};
    const bool s1CanRetire = old.s1Valid && s1Complete(collected);

    const ElasticAdvanceDecision advance = decideElasticAdvance({
        old.s0Valid,
        old.s1Valid,
        old.s2Valid,
        s1CanRetire,
        fifoPushReady ? 0 : StreamingFifoDepth,
        false,
        false,
    });
    const bool s2Ready = !old.s2Valid || fifoPushReady;
    const bool s2Fire = old.s2Valid && fifoPushReady;
    const bool s1Ready = !old.s1Valid ||
        (s1CanRetire && s2Ready);
    const bool s1Fire = old.s1Valid && s1CanRetire && s2Ready;
    const bool s0Ready = !old.s0Valid || s1Ready;
    const bool s0Fire = old.s0Valid && s1Ready;
    if (advance.s2Ready != s2Ready || advance.s1Ready != s1Ready ||
        advance.s0Ready != s0Ready) {
        throw std::logic_error("streaming ready contract diverged");
    }

    SramRequest request = responseRequest;
    SramRequest grant = responseRequest;
    if (shared) {
        request = {};
        if (old.s1Valid && !s1CanRetire) {
            request = arbitrateS1(collected);
        } else if (s0Fire) {
            request = arbitrateS1(beginS1(old.s0));
        }
        grant = (*grantFunction)(request);
        validateSharedGrant(request, grant);
    }

    PipelinedIm2ColCycle observation;
    observation.cycle = cycleNumber;
    observation.s0Valid = old.s0Valid;
    observation.s0Ready = s0Ready;
    observation.s0Fire = s0Fire;
    observation.s1Valid = old.s1Valid;
    observation.s1CanRetire = s1CanRetire;
    observation.s1Ready = s1Ready;
    observation.s1Fire = s1Fire;
    observation.s2Valid = old.s2Valid;
    observation.s2Ready = s2Ready;
    observation.s2Fire = s2Fire;
    observation.s0 = old.s0;
    observation.s1 = old.s1;
    observation.s2 = old.s2;
    observation.responseRequest = responseRequest;
    observation.request = request;
    observation.grant = grant;
    observation.response = response;
    observation.output = old.s2Valid ?
        StreamingFifoEntry{old.s2.tag, old.s2.compacted} :
        StreamingFifoEntry{};
    observation.producerExhausted = old.producerExhausted;

    if (old.s0Valid && !s1Ready) {
        counters.s0StallCycles = checkedAdd(
            counters.s0StallCycles, 1, "streaming S0 stall count");
    }
    if (old.s1Valid && !s1Fire) {
        counters.s1StallCycles = checkedAdd(
            counters.s1StallCycles, 1, "streaming S1 stall count");
        if (!s1CanRetire) {
            counters.bankConflictStallCycles = checkedAdd(
                counters.bankConflictStallCycles, 1,
                "streaming bank-conflict stall count");
        }
    }
    if (old.s2Valid && !fifoPushReady) {
        downstreamStallSinceLastPush = true;
        counters.s2StallCycles = checkedAdd(
            counters.s2StallCycles, 1, "streaming S2 stall count");
    }

    Registers next = old;
    if (old.s1Valid) {
        next.s1 = collected;
    }
    if (s2Fire) {
        next.s2Valid = false;
        notePushed(old.s2);
    }
    if (s1Fire) {
        next.s2Valid = true;
        next.s2 = finishS2(collected);
        next.s1Valid = false;
        noteRetiredS1(collected);
        if (!isCanonicalPrefixMask(collected.rawSpatialMask)) {
            counters.rawScatteredMaskVectors = checkedAdd(
                counters.rawScatteredMaskVectors, 1,
                "streaming scattered raw-mask vector count");
        }
    }
    if (s0Fire) {
        next.s1Valid = true;
        next.s1 = beginS1(old.s0);
        next.s0Valid = false;

        Iterators following = old.iterators;
        const bool exhausted = advanceIterators(following);
        if (exhausted) {
            next.iterators = following;
            next.producerExhausted = true;
        } else {
            next.iterators = following;
            next.s0 = buildS0Payload(following);
            next.s0Valid = true;
            noteGenerated(next.s0);
            observation.producerFire = true;
        }
    } else if (!old.s0Valid && !old.producerExhausted) {
        next.s0 = buildS0Payload(old.iterators);
        next.s0Valid = true;
        noteGenerated(next.s0);
        observation.producerFire = true;
    }

    registers = next;
    if (shared) {
        sharedInFlight = grant;
    }
    checkInvariants(registers);
    const bool drained = registers.producerExhausted &&
        !registers.s0Valid && !registers.s1Valid && !registers.s2Valid &&
        !hasPendingSharedRead();
    if (drained) {
        if (counters.inputVectors != dimensions.im2col.expectedVectors ||
            counters.outputVectors != dimensions.im2col.expectedVectors) {
            throw std::logic_error(
                "streaming model drained before all vectors transferred");
        }
        const uint64_t expectedPairs =
            dimensions.im2col.expectedVectors - 1;
        if (counters.pipelineFillCycles == 0 ||
            counters.producerInputPairs != expectedPairs ||
            counters.im2colOutputPairs != expectedPairs ||
            counters.conflictFreeOutputPairs > expectedPairs ||
            (counters.conflictFreeOutputPairs == 0 &&
             (counters.conflictFreeOutputGapCycles != 0 ||
              counters.conflictFreeOutputMaxGap != 0))) {
            throw std::logic_error(
                "streaming interval statistics failed at drained");
        }
        if (!lastGeneratedTag || !lastGeneratedTag->tileLast ||
            lastGeneratedTag->tileIndex + 1 != dimensions.expectedTiles ||
            !lastPushedTag || !lastPushedTag->tileLast ||
            lastPushedTag->tileIndex + 1 != dimensions.expectedTiles) {
            throw std::logic_error(
                "streaming model drained with incomplete tile sequence");
        }
        drainedAt = cycleNumber;
        observation.drained = true;
    }
    cycleNumber = checkedAdd(cycleNumber, 1, "streaming model cycle");
    return observation;
}

} // namespace gem5::sau_n
