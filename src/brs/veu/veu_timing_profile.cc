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
    const std::vector<std::string> required = {
        "profile_id", "op", "mode", "scalar_en", "mask_class",
        "source_set", "vfu_latency", "vfu_ii", "write_policy",
        "fifo_depth", "max_outstanding_reads", "vsu_latency"};
    if (header != required) {
        throw std::runtime_error("invalid VEU timing profile header: " + path);
    }

    uint32_t lineNumber = 1;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto fields = splitCsv(line);
        if (fields.size() != required.size()) {
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
        row.latency = positiveCycle(fields[6], "vfu_latency", lineNumber);
        row.initiationInterval = positiveCycle(fields[7], "vfu_ii", lineNumber);
        row.writePolicy = fields[8];
        row.fifoDepth = positiveCycle(fields[9], "fifo_depth", lineNumber);
        row.maxOutstandingReads = positiveCycle(
            fields[10], "max_outstanding_reads", lineNumber);
        row.vsuLatency = positiveCycle(fields[11], "vsu_latency", lineNumber);
        if (row.profileId.empty() || row.op.empty() || row.writePolicy.empty()) {
            throw std::runtime_error("empty required VEU timing profile field at line " +
                                     std::to_string(lineNumber));
        }
        rows.push_back(std::move(row));
    }
    if (rows.empty()) {
        throw std::runtime_error("VEU timing profile contains no data rows: " + path);
    }
}

VeuTimingSelection
VeuTimingProfile::select(const std::string &op, uint32_t mode,
                         bool scalarEnabled, const std::string &maskClass,
                         const std::string &sourceSet,
                         uint32_t fallbackLatency, uint32_t fallbackII,
                         uint32_t fallbackFifoDepth,
                         uint32_t fallbackMaxOutstanding,
                         uint32_t fallbackVsuLatency) const
{
    const std::string modeText = std::to_string(mode);
    const std::string scalarText = scalarEnabled ? "1" : "0";
    const Row *best = nullptr;
    int bestSpecificity = -1;
    for (const auto &row : rows) {
        if (!matches(row.op, op) || !matches(row.mode, modeText) ||
            !matches(row.scalarEnabled, scalarText) ||
            !matches(row.maskClass, maskClass) ||
            !matches(row.sourceSet, sourceSet)) {
            continue;
        }
        const int specificity = (row.op != "*") + (row.mode != "*") +
            (row.scalarEnabled != "*") + (row.maskClass != "*") +
            (row.sourceSet != "*");
        if (specificity > bestSpecificity) {
            best = &row;
            bestSpecificity = specificity;
        }
    }

    if (!best) {
        return {fallbackLatency, fallbackII, fallbackFifoDepth,
                fallbackMaxOutstanding, fallbackVsuLatency,
                "fallback", false};
    }
    return {best->latency, best->initiationInterval, best->fifoDepth,
            best->maxOutstandingReads, best->vsuLatency,
            best->profileId, true};
}

} // namespace brs
} // namespace gem5
