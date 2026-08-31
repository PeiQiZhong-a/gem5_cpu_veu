#include "sau_n/streaming_pipeline_contract.hh"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gem5::sau_n
{
namespace
{

uint64_t
maskPopulation(uint16_t mask)
{
    uint64_t count = 0;
    while (mask != 0) {
        count += mask & 1U;
        mask >>= 1;
    }
    return count;
}

uint16_t
prefixMask(uint64_t count)
{
    if (count > SauRows) {
        throw std::out_of_range("prefix mask count exceeds SA rows");
    }
    return count == SauRows ? std::numeric_limits<uint16_t>::max() :
        static_cast<uint16_t>((uint32_t{1} << count) - 1);
}

void
validateRawCoordinate(
    const StreamingSpatialCoordinate &coordinate, uint64_t lane,
    bool expectedValid)
{
    if (coordinate.valid != expectedValid) {
        throw std::invalid_argument(
            "raw spatial mask and coordinate validity disagree");
    }
    if (!coordinate.valid &&
        (coordinate.n != 0 || coordinate.oh != 0 || coordinate.ow != 0)) {
        throw std::invalid_argument(
            "invalid raw coordinate must be canonical zero");
    }
    if (lane >= SauRows) {
        throw std::out_of_range("raw spatial lane exceeds SA rows");
    }
}

void
requireRange(
    uint64_t value, uint64_t minimum, uint64_t maximum,
    std::string_view field)
{
    if (value < minimum || value > maximum) {
        throw std::invalid_argument(
            std::string(field) + " must be in [" +
            std::to_string(minimum) + ", " + std::to_string(maximum) + "]");
    }
}

void
requireResolvedSharedSpad(const PipelineDerivedConfig &derived)
{
    if (!derived.sharedSpad.configured) {
        throw std::invalid_argument(
            "shared scratchpad addresses require streaming-derived config");
    }
}

} // anonymous namespace

PipelineDerivedConfig
validateStreamingConfig(const PipelineResolvedConfig &config)
{
    auto derived = validateAndDerive(config);
    if (config.im2col.strideH != config.im2col.strideW ||
        (config.im2col.strideH != 1 && config.im2col.strideH != 2)) {
        throw std::invalid_argument(
            "streaming stride_h/stride_w must be equal and in {1, 2}");
    }
    if (config.im2col.padTop != config.im2col.padLeft ||
        config.im2col.padTop > 1) {
        throw std::invalid_argument(
            "streaming pad_top/pad_left must be equal and in {0, 1}");
    }
    if (config.im2col.dilationH != 1 || config.im2col.dilationW != 1) {
        throw std::invalid_argument(
            "streaming dilation_h/dilation_w must both be 1");
    }
    derived.sharedSpad = resolveSharedSpadConfig(config, derived);
    return derived;
}

SharedSpadConfig
resolveSharedSpadConfig(
    const PipelineResolvedConfig &config,
    const PipelineDerivedConfig &derived)
{
    SharedSpadConfig shared = config.sharedSpad;
    const uint64_t requiredD = checkedMultiply(
        checkedMultiply(
            config.im2col.n, config.im2col.outH, "D region rows"),
        config.im2col.outW, "D region rows");
    if (!shared.configured) {
        shared.configured = true;
        shared.aBase = config.im2col.spadBase;
        shared.aRows = derived.im2col.totalSpatialWords;
        shared.bBase = checkedAdd(
            shared.aBase, shared.aRows, "default B base");
        shared.bRows = derived.k;
        shared.cBase = checkedAdd(
            shared.bBase, shared.bRows, "default C base");
        shared.cRows = 2;
        shared.dBase = checkedAdd(
            shared.cBase, shared.cRows, "default D base");
        shared.dRows = requiredD;
        shared.bBufferDepth = derived.k;
        shared.dPendingRows = 1;
        shared.weightReuse = true;
        shared.arbitration = BankArbitrationPolicy::ADB;
    }

    struct Region
    {
        uint64_t base;
        uint64_t end;
        std::string_view name;
    };
    const std::array<std::pair<std::string_view, uint64_t>, 4> required = {{
        {"A", derived.im2col.totalSpatialWords},
        {"B", derived.k},
        {"C", 2},
        {"D", requiredD},
    }};
    const std::array<uint64_t, 4> bases = {
        shared.aBase, shared.bBase, shared.cBase, shared.dBase};
    const std::array<uint64_t, 4> rows = {
        shared.aRows, shared.bRows, shared.cRows, shared.dRows};
    std::vector<Region> regions;
    for (std::size_t index = 0; index < required.size(); ++index) {
        const std::string label =
            "shared scratchpad " + std::string(required[index].first);
        requireRange(
            bases[index], 0, SpBankEntries - 1, label + " base");
        requireRange(
            rows[index], required[index].second, SpBankEntries,
            label + " rows");
        const uint64_t end = checkedAdd(
            bases[index], rows[index], label + " end");
        if (end > SpBankEntries) {
            throw std::invalid_argument(label + " region exceeds 4096 rows");
        }
        regions.push_back({bases[index], end, required[index].first});
    }
    if (shared.aBase != config.im2col.spadBase) {
        throw std::invalid_argument(
            "shared scratchpad A base must equal im2col.spad_base");
    }
    std::sort(
        regions.begin(), regions.end(),
        [](const Region &left, const Region &right) {
            return left.base < right.base;
        });
    for (std::size_t index = 1; index < regions.size(); ++index) {
        if (regions[index].base < regions[index - 1].end) {
            throw std::invalid_argument(
                std::string("shared scratchpad regions overlap: ") +
                std::string(regions[index - 1].name) + " and " +
                std::string(regions[index].name));
        }
    }
    requireRange(
        shared.bBufferDepth, 1, derived.k, "B buffer depth");
    requireRange(
        shared.dPendingRows, 1, std::numeric_limits<uint64_t>::max(),
        "D pending rows");
    if (shared.arbitration != BankArbitrationPolicy::ADB) {
        throw std::invalid_argument(
            "unsupported shared scratchpad arbitration policy");
    }
    return shared;
}

ScratchpadAddress
bAddress(
    const PipelineResolvedConfig &config,
    const PipelineDerivedConfig &derived,
    uint64_t kIndex, uint64_t outputChannel)
{
    requireResolvedSharedSpad(derived);
    requireRange(kIndex, 0, derived.k - 1, "B K index");
    requireRange(
        outputChannel, 0, config.outChannels - 1, "B output channel");
    return {outputChannel, derived.sharedSpad.bBase + kIndex};
}

ScratchpadAddress
cAddress(
    const PipelineResolvedConfig &config,
    const PipelineDerivedConfig &derived,
    uint64_t outputChannel, uint64_t byteIndex)
{
    requireResolvedSharedSpad(derived);
    requireRange(
        outputChannel, 0, config.outChannels - 1, "C output channel");
    requireRange(byteIndex, 0, 1, "C byte index");
    return {outputChannel, derived.sharedSpad.cBase + byteIndex};
}

ScratchpadAddress
dAddress(
    const PipelineResolvedConfig &config,
    const PipelineDerivedConfig &derived,
    uint64_t n, uint64_t oh, uint64_t ow, uint64_t outputChannel)
{
    requireResolvedSharedSpad(derived);
    requireRange(n, 0, config.im2col.n - 1, "D n");
    requireRange(oh, 0, config.im2col.outH - 1, "D output h");
    requireRange(ow, 0, config.im2col.outW - 1, "D output w");
    requireRange(
        outputChannel, 0, config.outChannels - 1, "D output channel");
    uint64_t spatial = checkedMultiply(n, config.im2col.outH, "D address");
    spatial = checkedAdd(spatial, oh, "D address");
    spatial = checkedMultiply(spatial, config.im2col.outW, "D address");
    spatial = checkedAdd(spatial, ow, "D address");
    return {outputChannel, derived.sharedSpad.dBase + spatial};
}

bool
isCanonicalPrefixMask(uint16_t mask)
{
    return mask == prefixMask(maskPopulation(mask));
}

CompactedSpatialPayload
compactSpatialPayload(const RawSpatialPayload &raw)
{
    if (raw.spatialMask == 0) {
        throw std::invalid_argument("raw spatial mask must be nonzero");
    }

    CompactedSpatialPayload compacted;
    for (uint64_t source = 0; source < SauRows; ++source) {
        const bool valid =
            (raw.spatialMask & (uint16_t{1} << source)) != 0;
        validateRawCoordinate(raw.coordinates[source], source, valid);
        if (!valid) {
            if (raw.activations[source] != 0) {
                throw std::invalid_argument(
                    "invalid raw activation lane must be canonical zero");
            }
            continue;
        }
        for (uint64_t earlier = 0; earlier < source; ++earlier) {
            if ((raw.spatialMask & (uint16_t{1} << earlier)) != 0 &&
                raw.coordinates[earlier] == raw.coordinates[source]) {
                throw std::invalid_argument(
                    "raw spatial coordinates must be unique");
            }
        }
        const uint64_t destination = compacted.validRows;
        compacted.activations[destination] = raw.activations[source];
        compacted.sourceLanes[destination] =
            static_cast<uint8_t>(source);
        compacted.coordinates[destination] = raw.coordinates[source];
        ++compacted.validRows;
    }
    compacted.spatialMask = prefixMask(compacted.validRows);
    return compacted;
}

void
validateVectorTag(const StreamingVectorTag &tag, uint64_t k)
{
    if (k == 0 || k > SauMaxChannels * 9 || k % 9 != 0) {
        throw std::invalid_argument(
            "streaming K must be a nonzero multiple of 9");
    }
    if (tag.ocGroup != 0) {
        throw std::invalid_argument("streaming oc_group must be zero");
    }
    if (tag.validColumns == 0 || tag.validColumns > SauColumns) {
        throw std::invalid_argument(
            "streaming valid_columns must be in [1, 16]");
    }
    if (tag.c >= k / 9 || tag.kh >= 3 || tag.kw >= 3 ||
        tag.kIndex >= k) {
        throw std::invalid_argument("streaming vector tag exceeds K bounds");
    }
    const uint64_t expectedK = tag.c * 9 + tag.kh * 3 + tag.kw;
    if (tag.kIndex != expectedK) {
        throw std::invalid_argument("streaming vector tag K order mismatch");
    }
    if (tag.tileFirst != (tag.kIndex == 0) ||
        tag.tileLast != (tag.kIndex + 1 == k)) {
        throw std::invalid_argument("streaming tile boundary tag mismatch");
    }
}

void
validateVectorSequence(
    const StreamingVectorTag &previousTag,
    const StreamingVectorTag &currentTag,
    uint64_t k)
{
    validateVectorTag(previousTag, k);
    validateVectorTag(currentTag, k);
    if (currentTag.tileIndex == previousTag.tileIndex) {
        if (previousTag.tileLast ||
            currentTag.kIndex != previousTag.kIndex + 1) {
            throw std::invalid_argument(
                "streaming K sequence is not contiguous within a tile");
        }
        return;
    }
    if (!previousTag.tileLast || !currentTag.tileFirst ||
        currentTag.tileIndex != previousTag.tileIndex + 1) {
        throw std::invalid_argument(
            "streaming tile sequence is not contiguous");
    }
}

void
validateSameTileMetadata(
    const StreamingVectorTag &previousTag,
    const CompactedSpatialPayload &previousPayload,
    const StreamingVectorTag &currentTag,
    const CompactedSpatialPayload &currentPayload)
{
    if (previousTag.tileIndex != currentTag.tileIndex ||
        previousTag.ocGroup != currentTag.ocGroup ||
        previousTag.validColumns != currentTag.validColumns ||
        previousPayload.spatialMask != currentPayload.spatialMask ||
        previousPayload.validRows != currentPayload.validRows ||
        previousPayload.sourceLanes != currentPayload.sourceLanes ||
        previousPayload.coordinates != currentPayload.coordinates) {
        throw std::invalid_argument(
            "streaming metadata changed within one tile");
    }
}

ElasticAdvanceDecision
decideElasticAdvance(const ElasticAdvanceInputs &inputs)
{
    if (inputs.fifoCount > StreamingFifoDepth) {
        throw std::out_of_range("streaming FIFO count exceeds depth");
    }
    if (inputs.fifoPop && inputs.fifoCount == 0) {
        throw std::logic_error("streaming FIFO cannot pop while empty");
    }

    ElasticAdvanceDecision decision;
    decision.fifoPushReady =
        inputs.fifoCount < StreamingFifoDepth || inputs.fifoPop;
    decision.s2Ready = !inputs.s2Valid || decision.fifoPushReady;
    decision.s1Ready = !inputs.s1Valid ||
        (inputs.s1CanRetire && decision.s2Ready);
    decision.s0Ready = !inputs.s0Valid || decision.s1Ready;
    decision.producerReady = decision.s0Ready && inputs.moreVectors;
    decision.s2ToFifo = inputs.s2Valid && decision.fifoPushReady;
    decision.s1ToS2 =
        inputs.s1Valid && inputs.s1CanRetire && decision.s2Ready;
    decision.s0ToS1 = inputs.s0Valid && decision.s1Ready;
    decision.producerToS0 = inputs.moreVectors && decision.s0Ready;
    return decision;
}

ElasticFifoDecision
decideElasticFifo(uint64_t count, bool pushValid, bool popRequest)
{
    if (count > StreamingFifoDepth) {
        throw std::out_of_range("streaming FIFO count exceeds depth");
    }
    if (popRequest && count == 0) {
        throw std::logic_error("streaming FIFO cannot pop while empty");
    }
    ElasticFifoDecision decision;
    decision.pushReady = count < StreamingFifoDepth || popRequest;
    decision.push = pushValid && decision.pushReady;
    decision.pop = popRequest;
    decision.nextCount = count + (decision.push ? 1 : 0) -
        (decision.pop ? 1 : 0);
    if (decision.nextCount > StreamingFifoDepth) {
        throw std::logic_error("streaming FIFO conservation failed");
    }
    return decision;
}

DPendingQueueDecision
decideDPendingQueue(
    uint64_t occupancy, uint64_t depth, uint16_t headPendingMask,
    uint16_t writeGrantMask, bool outputReady)
{
    if (depth == 0 || occupancy > depth) {
        throw std::out_of_range("invalid D pending queue occupancy/depth");
    }
    if (occupancy == 0) {
        if (headPendingMask != 0 || writeGrantMask != 0) {
            throw std::logic_error(
                "empty D pending queue cannot have a head or write grants");
        }
    } else {
        if (headPendingMask == 0) {
            throw std::logic_error(
                "occupied D pending queue requires a pending head");
        }
        if ((writeGrantMask & ~headPendingMask) != 0) {
            throw std::logic_error(
                "D write grant must be a subset of the pending head");
        }
    }

    DPendingQueueDecision decision;
    decision.headWillRetire =
        occupancy != 0 &&
        (headPendingMask & ~writeGrantMask) == 0;
    decision.pushReady =
        occupancy < depth || decision.headWillRetire;
    decision.outputGrant = outputReady && decision.pushReady;
    return decision;
}

SharedSpadArbitrationDecision
arbitrateSharedSpad(
    const SramRequest &aRequest,
    const SramRequest &bRequest,
    const SramRequest &cRequest,
    const SramRequest &dRequest)
{
    const auto hasRequest = [](const SramRequest &request) {
        return std::any_of(
            request.valid.begin(), request.valid.end(),
            [](bool valid) { return valid; });
    };
    if (hasRequest(cRequest) &&
        (hasRequest(aRequest) || hasRequest(bRequest) ||
         hasRequest(dRequest))) {
        throw std::logic_error(
            "C initialization must be exclusive of A/B/D requests");
    }

    SharedSpadArbitrationDecision decision;
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        const SramRequest *winner = nullptr;
        SramRequest *grant = nullptr;
        if (cRequest.valid[bank]) {
            winner = &cRequest;
            grant = &decision.cGrant;
        } else if (aRequest.valid[bank]) {
            winner = &aRequest;
            grant = &decision.aGrant;
        } else if (dRequest.valid[bank]) {
            winner = &dRequest;
            grant = &decision.dGrant;
        } else if (bRequest.valid[bank]) {
            winner = &bRequest;
            grant = &decision.bGrant;
        }
        if (winner == nullptr) {
            continue;
        }
        grant->valid[bank] = true;
        grant->address[bank] = winner->address[bank];
        if (grant != &decision.dGrant) {
            decision.readGrant.valid[bank] = true;
            decision.readGrant.address[bank] = winner->address[bank];
        }
    }
    return decision;
}

StreamingConsumerDecision
decideStreamingConsumer(
    StreamingConsumerState state,
    bool fifoValid,
    const StreamingVectorTag &headTag,
    uint64_t activeTile,
    uint64_t acceptedK,
    uint64_t k)
{
    if (acceptedK > k) {
        throw std::out_of_range("accepted K exceeds configured K");
    }
    StreamingConsumerDecision decision;
    switch (state) {
      case StreamingConsumerState::Idle:
        if (fifoValid) {
            validateVectorTag(headTag, k);
            if (!headTag.tileFirst || headTag.kIndex != 0) {
                throw std::logic_error(
                    "idle consumer requires a tile-first FIFO head");
            }
            decision.beginLaunch = true;
        }
        break;
      case StreamingConsumerState::Launch:
        if (!fifoValid || headTag.tileIndex != activeTile ||
            !headTag.tileFirst || headTag.kIndex != 0) {
            throw std::logic_error(
                "launch consumer must retain the tile-first FIFO head");
        }
        validateVectorTag(headTag, k);
        decision.launch = true;
        break;
      case StreamingConsumerState::AcceptK:
        decision.peReady = acceptedK < k;
        if (fifoValid) {
            validateVectorTag(headTag, k);
            if (headTag.tileIndex != activeTile ||
                headTag.kIndex != acceptedK) {
                throw std::logic_error(
                    "FIFO head does not match active tile/K");
            }
            decision.inputValid = true;
        }
        decision.inputFire = decision.inputValid && decision.peReady;
        break;
      case StreamingConsumerState::WaitResult:
      case StreamingConsumerState::DrainOutput:
        break;
    }
    return decision;
}

SauInputCycleDecision
decideSauInputCycle(
    SauInputProtocol protocol,
    bool streamActive,
    uint64_t acceptedInputs,
    uint64_t calcCycles,
    bool inputFire,
    bool previouslyScheduledMacDue)
{
    if (calcCycles == 0 || calcCycles > SauMaxChannels * 9 ||
        acceptedInputs > calcCycles) {
        throw std::invalid_argument("invalid SA input progress");
    }
    if (inputFire && acceptedInputs == calcCycles) {
        throw std::logic_error("SA input exceeds configured K");
    }
    if (protocol == SauInputProtocol::StrictRtlContinuous &&
        streamActive && acceptedInputs != 0 &&
        acceptedInputs < calcCycles && !inputFire) {
        throw std::logic_error(
            "strict SA input stream cannot contain bubbles");
    }
    return {
        inputFire,
        previouslyScheduledMacDue,
        acceptedInputs + (inputFire ? 1 : 0),
    };
}

void
validateDrainedConservation(const StreamingConservationCounts &counts)
{
    if (counts.expectedVectors == 0 || counts.expectedTiles == 0) {
        throw std::invalid_argument(
            "streaming expected vector/tile counts must be nonzero");
    }
    if (counts.producerAccepted != counts.expectedVectors ||
        counts.s2Pushed != counts.expectedVectors ||
        counts.fifoPushed != counts.expectedVectors ||
        counts.fifoPopped != counts.expectedVectors ||
        counts.peAccepted != counts.expectedVectors) {
        throw std::logic_error(
            "streaming vector conservation failed at drained");
    }
    if (counts.tilesGenerated != counts.expectedTiles ||
        counts.tilesLaunched != counts.expectedTiles ||
        counts.tilesCompleted != counts.expectedTiles) {
        throw std::logic_error(
            "streaming tile conservation failed at drained");
    }
}

} // namespace gem5::sau_n
