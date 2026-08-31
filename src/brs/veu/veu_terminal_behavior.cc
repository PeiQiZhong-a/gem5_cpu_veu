#include "brs/veu/veu_terminal_behavior.hh"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

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
parseCycle(const std::string &value, const std::string &column,
           uint32_t line, bool allowZero)
{
    size_t consumed = 0;
    unsigned long parsed = 0;
    try {
        parsed = std::stoul(value, &consumed, 0);
    } catch (const std::exception &) {
        throw std::runtime_error("invalid " + column + " at line " +
                                 std::to_string(line));
    }
    if (consumed != value.size() || (!allowZero && parsed == 0) ||
        parsed > 0xffffffffUL) {
        throw std::runtime_error("invalid " + column + " at line " +
                                 std::to_string(line));
    }
    return static_cast<uint32_t>(parsed);
}

} // anonymous namespace

void
VeuTerminalBehavior::load(const std::string &path)
{
    rows.clear();
    if (path.empty()) {
        return;
    }
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open VEU terminal behavior: " + path);
    }
    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("empty VEU terminal behavior: " + path);
    }
    const std::vector<std::string> header = {
        "behavior_id", "op", "scalar_en", "mask_class", "chunk_class",
        "classification", "status_clear_cycles", "lock_finish_cycles",
        "extra_vfu_accepts", "extra_writes", "tail_reads", "stuck",
        "evidence_id"};
    if (splitCsv(line) != header) {
        throw std::runtime_error("invalid VEU terminal behavior header: " +
                                 path);
    }

    std::unordered_set<std::string> keys;
    uint32_t lineNumber = 1;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto fields = splitCsv(line);
        if (fields.size() != header.size()) {
            throw std::runtime_error(
                "invalid VEU terminal behavior column count at line " +
                std::to_string(lineNumber));
        }
        if (fields[0].empty() || fields[1].empty() || fields[3].empty() ||
            fields[5].empty() || fields[12].empty()) {
            throw std::runtime_error(
                "empty required VEU terminal behavior field at line " +
                std::to_string(lineNumber));
        }
        if (fields[2] != "0" && fields[2] != "1") {
            throw std::runtime_error("invalid scalar_en at line " +
                                     std::to_string(lineNumber));
        }
        if (fields[3] != "full" && fields[3] != "partial" &&
            fields[3] != "zero") {
            throw std::runtime_error("invalid mask_class at line " +
                                     std::to_string(lineNumber));
        }
        if (fields[11] != "0" && fields[11] != "1") {
            throw std::runtime_error("invalid stuck at line " +
                                     std::to_string(lineNumber));
        }

        Row row;
        row.behaviorId = fields[0];
        row.op = fields[1];
        row.scalarEnabled = fields[2] == "1";
        row.maskClass = fields[3];
        row.chunkCount = parseCycle(fields[4], "chunk_class", lineNumber,
                                    false);
        row.selection.behaviorId = row.behaviorId;
        row.selection.classification = fields[5];
        row.selection.statusClearCycles = parseCycle(
            fields[6], "status_clear_cycles", lineNumber, false);
        row.selection.lockFinishCycles = parseCycle(
            fields[7], "lock_finish_cycles", lineNumber, false);
        row.selection.extraVfuAccepts = parseCycle(
            fields[8], "extra_vfu_accepts", lineNumber, true);
        row.selection.extraWrites = parseCycle(
            fields[9], "extra_writes", lineNumber, true);
        row.selection.tailReads = parseCycle(
            fields[10], "tail_reads", lineNumber, true);
        row.selection.stuck = fields[11] == "1";
        row.selection.evidenceId = fields[12];
        if (!row.selection.stuck &&
            row.selection.statusClearCycles >=
                row.selection.lockFinishCycles) {
            throw std::runtime_error(
                "terminal status must clear before lock finish at line " +
                std::to_string(lineNumber));
        }
        const std::string key = row.op + "|" + fields[2] + "|" +
            row.maskClass + "|" + fields[4];
        if (!keys.insert(key).second) {
            throw std::runtime_error(
                "duplicate VEU terminal behavior key at line " +
                std::to_string(lineNumber));
        }
        rows.push_back(std::move(row));
    }
    if (rows.empty()) {
        throw std::runtime_error(
            "VEU terminal behavior contains no data rows: " + path);
    }
}

std::optional<VeuTerminalSelection>
VeuTerminalBehavior::select(const std::string &op, bool scalarEnabled,
                            const std::string &maskClass,
                            uint32_t chunkCount) const
{
    for (const auto &row : rows) {
        if (row.op == op && row.scalarEnabled == scalarEnabled &&
            row.maskClass == maskClass && row.chunkCount == chunkCount) {
            return row.selection;
        }
    }
    return std::nullopt;
}

} // namespace brs
} // namespace gem5
