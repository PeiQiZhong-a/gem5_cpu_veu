#include "brs/veu/veu_timing_profile.hh"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace gem5
{
namespace brs
{

namespace
{

std::vector<std::string>
splitCsv(const std::string &line)
{
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) {
        fields.push_back(field);
    }
    if (!line.empty() && line.back() == ',') {
        fields.emplace_back();
    }
    return fields;
}

uint32_t
positiveCycle(const std::string &value, const std::string &column,
              uint32_t line)
{
    size_t consumed = 0;
    unsigned long parsed = 0;
    try {
        parsed = std::stoul(value, &consumed, 0);
    } catch (const std::exception &) {
        throw std::runtime_error("invalid " + column + " at line " +
                                 std::to_string(line));
    }
    if (consumed != value.size() || parsed == 0 || parsed > 0xffffffffUL) {
        throw std::runtime_error("invalid " + column + " at line " +
                                 std::to_string(line));
    }
    return static_cast<uint32_t>(parsed);
}

uint32_t
nonNegativeCycle(const std::string &value, const std::string &column,
                 uint32_t line)
{
    size_t consumed = 0;
    unsigned long parsed = 0;
    try {
        parsed = std::stoul(value, &consumed, 0);
    } catch (const std::exception &) {
        throw std::runtime_error("invalid " + column + " at line " +
                                 std::to_string(line));
    }
    if (consumed != value.size() || parsed > 0xffffffffUL) {
        throw std::runtime_error("invalid " + column + " at line " +
                                 std::to_string(line));
    }
    return static_cast<uint32_t>(parsed);
}

bool
matches(const std::string &pattern, const std::string &value)
{
    return pattern == "*" || pattern == value;
}

} // anonymous namespace

void
VeuTimingProfile::load(const std::string &path)
{
    rows.clear();
    if (path.empty()) {
        return;
    }

    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open VEU timing profile: " + path);
    }

    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("empty VEU timing profile: " + path);
    }
    const auto header = splitCsv(line);
    const std::vector<std::string> v1Header = {
        "profile_id", "op", "mode", "scalar_en", "mask_class",
        "source_set", "vfu_latency", "vfu_ii", "write_policy",
        "fifo_depth", "max_outstanding_reads", "vsu_latency"};
    const std::vector<std::string> v2Header = {
        "profile_id", "op", "mode", "scalar_en", "mask_class",
        "source_set", "chunk_class", "vfu_latency", "vfu_ii",
        "write_policy", "fifo_depth", "max_outstanding_reads",
        "vsu_latency", "timing_source", "evidence_id"};
    const std::vector<std::string> v3Header = {
        "profile_id", "op", "mode", "scalar_en", "mask_class",
        "source_set", "chunk_class", "vfu_latency", "vfu_ii",
        "write_policy", "fifo_depth", "max_outstanding_reads",
        "vsu_latency", "operation_cycles", "timing_source", "evidence_id"};
    const std::vector<std::string> v4Header = {
        "profile_id", "op", "mode", "scalar_en", "mask_class",
        "source_set", "chunk_class", "vfu_latency", "vfu_ii",
        "write_policy", "fifo_depth", "max_outstanding_reads",
        "vsu_latency", "lock_start_delay", "finish_drain_cycles",
        "operation_cycles", "timing_source", "evidence_id"};
    const bool v1 = header == v1Header;
    const bool v2 = header == v2Header;
    const bool v3 = header == v3Header;
    const bool v4 = header == v4Header;
    if (!v1 && !v2 && !v3 && !v4) {
        throw std::runtime_error("invalid VEU timing profile header: " + path);
    }

    uint32_t lineNumber = 1;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto fields = splitCsv(line);
        const size_t expectedColumns = v1 ? v1Header.size() :
            (v2 ? v2Header.size() : (v3 ? v3Header.size() : v4Header.size()));
        if (fields.size() != expectedColumns) {
            throw std::runtime_error("invalid VEU timing profile column count at line " +
                                     std::to_string(lineNumber));
        }
        Row row;
        row.profileId = fields[0];
        row.op = fields[1];
        row.mode = fields[2];
        row.scalarEnabled = fields[3];
        row.maskClass = fields[4];
        row.sourceSet = fields[5];
        const size_t offset = (v2 || v3 || v4) ? 1 : 0;
        if (v2 || v3 || v4) row.chunkClass = fields[6];
        row.latency = positiveCycle(fields[6 + offset], "vfu_latency",
                                    lineNumber);
        row.initiationInterval = positiveCycle(
            fields[7 + offset], "vfu_ii", lineNumber);
        row.writePolicy = fields[8 + offset];
        row.fifoDepth = positiveCycle(fields[9 + offset], "fifo_depth",
                                      lineNumber);
        row.maxOutstandingReads = positiveCycle(
            fields[10 + offset], "max_outstanding_reads", lineNumber);
        row.vsuLatency = positiveCycle(fields[11 + offset], "vsu_latency",
                                       lineNumber);
        if (v4) {
            row.lockStartDelay = nonNegativeCycle(
                fields[13], "lock_start_delay", lineNumber);
            row.finishDrainCycles = positiveCycle(
                fields[14], "finish_drain_cycles", lineNumber);
            row.operationCycles = positiveCycle(
                fields[15], "operation_cycles", lineNumber);
            row.timingSource = fields[16];
            row.evidenceId = fields[17];
            row.hasControlTiming = true;
        } else if (v3) {
            row.operationCycles = positiveCycle(
                fields[13], "operation_cycles", lineNumber);
            row.timingSource = fields[14];
            row.evidenceId = fields[15];
        } else if (v2) {
            row.timingSource = fields[13];
            row.evidenceId = fields[14];
        } else {
            row.evidenceId = "legacy:" + row.profileId;
        }
        if (row.profileId.empty() || row.op.empty() || row.mode.empty() ||
            row.writePolicy.empty() || row.chunkClass.empty() ||
            row.timingSource.empty() || row.evidenceId.empty()) {
            throw std::runtime_error("empty required VEU timing profile field at line " +
                                     std::to_string(lineNumber));
        }
        if (row.timingSource != "rtl_sim" &&
            row.timingSource != "legacy_default" &&
            row.timingSource != "default") {
            throw std::runtime_error("invalid timing_source at line " +
                                     std::to_string(lineNumber));
        }
        if (row.timingSource == "rtl_sim" &&
            (row.op == "*" || row.scalarEnabled == "*" ||
             row.maskClass == "*" ||
             row.sourceSet == "*" || row.chunkClass == "*")) {
            throw std::runtime_error(
                "rtl_sim timing row must identify an exact "
                "op/scalar/mask/source/chunk tuple at line " +
                std::to_string(lineNumber));
        }
        for (const auto &existing : rows) {
            const bool sameTimingKey =
                existing.op == row.op &&
                existing.scalarEnabled == row.scalarEnabled &&
                existing.maskClass == row.maskClass &&
                existing.sourceSet == row.sourceSet &&
                existing.chunkClass == row.chunkClass;
            const bool sameTiming =
                existing.latency == row.latency &&
                existing.initiationInterval == row.initiationInterval &&
                existing.writePolicy == row.writePolicy &&
                existing.fifoDepth == row.fifoDepth &&
                existing.maxOutstandingReads == row.maxOutstandingReads &&
                existing.vsuLatency == row.vsuLatency &&
                existing.lockStartDelay == row.lockStartDelay &&
                existing.finishDrainCycles == row.finishDrainCycles &&
                existing.operationCycles == row.operationCycles &&
                existing.hasControlTiming == row.hasControlTiming &&
                existing.timingSource == row.timingSource;
            if (sameTimingKey && !sameTiming) {
                throw std::runtime_error(
                    "conflicting mode-independent VEU timing rows " +
                    existing.profileId + " and " + row.profileId +
                    " at line " + std::to_string(lineNumber));
            }
        }
        rows.push_back(std::move(row));
    }
    if (rows.empty()) {
        throw std::runtime_error("VEU timing profile contains no data rows: " + path);
    }
}

VeuTimingSelection
VeuTimingProfile::select(const std::string &op, bool scalarEnabled,
                         const std::string &maskClass,
                         const std::string &sourceSet,
                         uint32_t chunkCount,
                         uint32_t fallbackLatency, uint32_t fallbackII,
                         uint32_t fallbackFifoDepth,
                         uint32_t fallbackMaxOutstanding,
                         uint32_t fallbackVsuLatency,
                         uint32_t fallbackLockStartDelay,
                         uint32_t fallbackFinishDrainCycles) const
{
    const std::string scalarText = scalarEnabled ? "1" : "0";
    const std::string chunkText = std::to_string(chunkCount);
    const Row *best = nullptr;
    int bestSpecificity = -1;
    for (const auto &row : rows) {
        if (!matches(row.op, op) ||
            !matches(row.scalarEnabled, scalarText) ||
            !matches(row.maskClass, maskClass) ||
            !matches(row.sourceSet, sourceSet) ||
            !matches(row.chunkClass, chunkText)) {
            continue;
        }
        const int specificity = (row.op != "*") +
            (row.scalarEnabled != "*") + (row.maskClass != "*") +
            (row.sourceSet != "*") + (row.chunkClass != "*");
        if (specificity > bestSpecificity) {
            best = &row;
            bestSpecificity = specificity;
        }
    }

    VeuTimingSelection result;
    if (!best) {
        result.latency = fallbackLatency;
        result.initiationInterval = fallbackII;
        result.fifoDepth = fallbackFifoDepth;
        result.maxOutstandingReads = fallbackMaxOutstanding;
        result.vsuLatency = fallbackVsuLatency;
        result.lockStartDelay = fallbackLockStartDelay;
        result.finishDrainCycles = fallbackFinishDrainCycles;
        return result;
    }
    result.latency = best->latency;
    result.initiationInterval = best->initiationInterval;
    result.fifoDepth = best->fifoDepth;
    result.maxOutstandingReads = best->maxOutstandingReads;
    result.vsuLatency = best->vsuLatency;
    result.lockStartDelay = best->hasControlTiming ?
        best->lockStartDelay : fallbackLockStartDelay;
    result.finishDrainCycles = best->hasControlTiming ?
        best->finishDrainCycles : fallbackFinishDrainCycles;
    result.operationCycles = best->operationCycles;
    result.profileId = best->profileId;
    result.timingSource = best->timingSource;
    result.evidenceId = best->evidenceId;
    result.controlTimingSource = best->hasControlTiming ?
        best->timingSource : "default";
    result.controlEvidenceId = best->hasControlTiming ?
        best->evidenceId : "builtin_veu_timing_config";
    result.matched = true;
    return result;
}

} // namespace brs
} // namespace gem5
