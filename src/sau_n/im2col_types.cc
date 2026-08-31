#include "sau_n/im2col_types.hh"

#include <limits>
#include <stdexcept>
#include <string>

namespace gem5::sau_n
{
namespace
{

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

uint64_t
rowsPerWord(uint64_t w)
{
    return w <= BlockSize ? BlockSize / w : 1;
}

} // anonymous namespace

std::string_view
stateName(Im2ColState state)
{
    switch (state) {
      case Im2ColState::Idle:
        return "IDLE";
      case Im2ColState::Issue:
        return "ISSUE";
      case Im2ColState::Collect:
        return "COLLECT";
      case Im2ColState::Push:
        return "PUSH";
      case Im2ColState::Next:
        return "NEXT";
      case Im2ColState::Done:
        return "DONE";
    }
    throw std::invalid_argument("invalid Im2Col state value");
}

uint64_t
checkedAdd(uint64_t lhs, uint64_t rhs, std::string_view description)
{
    if (lhs > std::numeric_limits<uint64_t>::max() - rhs) {
        throw std::invalid_argument(std::string(description) + " overflows");
    }
    return lhs + rhs;
}

uint64_t
checkedMultiply(uint64_t lhs, uint64_t rhs, std::string_view description)
{
    if (rhs != 0 && lhs > std::numeric_limits<uint64_t>::max() / rhs) {
        throw std::invalid_argument(std::string(description) + " overflows");
    }
    return lhs * rhs;
}

uint64_t
ceilDivide(uint64_t value, uint64_t divisor)
{
    if (divisor == 0) {
        throw std::invalid_argument("ceilDivide divisor must be nonzero");
    }
    return value / divisor + (value % divisor != 0);
}

DerivedConfig
validateAndDerive(const ResolvedConfig &config)
{
    if (config.schemaVersion != SchemaVersion) {
        throw std::invalid_argument("schema_version must be 1");
    }
    if (config.inputGenerator != "tb_act_value_v1") {
        throw std::invalid_argument(
            "input_generator must be tb_act_value_v1");
    }

    requireRange(config.n, 1, 65535, "n");
    requireRange(config.c, 1, 65535, "c");
    requireRange(config.h, 1, 65535, "h");
    requireRange(config.w, 1, 65535, "w");
    requireRange(config.outH, 1, 65535, "out_h");
    requireRange(config.outW, 1, 65535, "out_w");
    requireRange(config.padTop, 0, 65535, "pad_top");
    requireRange(config.padLeft, 0, 65535, "pad_left");
    requireRange(config.kernelH, 1, 15, "kernel_h");
    requireRange(config.kernelW, 1, 15, "kernel_w");
    requireRange(config.strideH, 1, 15, "stride_h");
    requireRange(config.strideW, 1, 15, "stride_w");
    requireRange(config.dilationH, 1, 15, "dilation_h");
    requireRange(config.dilationW, 1, 15, "dilation_w");
    requireRange(config.spadBase, 0, SpBankEntries - 1, "spad_base");

    const uint64_t kernelArea = checkedMultiply(
        config.kernelH, config.kernelW, "kernel area");
    if (kernelArea > BlockSize) {
        throw std::invalid_argument("kernel_h * kernel_w must be <= 16");
    }
    if (config.cfgDwMode != 0) {
        throw std::invalid_argument("cfg_dw_mode must be 0");
    }
    if (config.cfgKernelPattern != KernelPatternAll) {
        throw std::invalid_argument("cfg_kernel_pattern must be 0xffff");
    }
    if (config.w <= BlockSize && config.outW > config.w) {
        throw std::invalid_argument("out_w must be <= w when w <= 16");
    }

    DerivedConfig derived;
    derived.rowsPerWord = rowsPerWord(config.w);
    derived.wWords = ceilDivide(config.w, BlockSize);
    derived.spatialWordsPerChannel = config.w <= BlockSize ?
        ceilDivide(config.h, derived.rowsPerWord) :
        checkedMultiply(config.h, derived.wWords, "spatial word count");
    derived.totalSpatialWords = checkedMultiply(
        checkedMultiply(config.n, config.c, "total spatial word count"),
        derived.spatialWordsPerChannel, "total spatial word count");

    const uint64_t footprintEnd = checkedAdd(
        config.spadBase, derived.totalSpatialWords,
        "scratchpad footprint");
    if (footprintEnd > SpBankEntries) {
        throw std::invalid_argument(
            "spad_base + total_spatial_words must be <= 4096");
    }

    derived.hGroups = config.w <= BlockSize ?
        ceilDivide(config.outH, derived.rowsPerWord) : config.outH;
    derived.wGroups = config.w <= BlockSize ?
        1 : ceilDivide(config.outW, BlockSize);

    uint64_t expected = checkedMultiply(
        config.n, config.c, "expected vector count");
    expected = checkedMultiply(
        expected, config.kernelH, "expected vector count");
    expected = checkedMultiply(
        expected, config.kernelW, "expected vector count");
    expected = checkedMultiply(
        expected, derived.hGroups, "expected vector count");
    derived.expectedVectors = checkedMultiply(
        expected, derived.wGroups, "expected vector count");
    return derived;
}

bool
busy(const ControlRegisters &registers)
{
    return registers.state != Im2ColState::Idle;
}

ControlRegisters
beginNext(const ControlRegisters &oldRegisters)
{
    ControlRegisters next = oldRegisters;
    next.done = false;
    return next;
}

void
commit(
    ControlRegisters &oldRegisters, const ControlRegisters &nextRegisters)
{
    oldRegisters = nextRegisters;
}

ControlRegisters
cycleZeroRegisters()
{
    return {Im2ColState::Issue, false};
}

ControlRegisters
doneTransitionNext(const ControlRegisters &oldRegisters)
{
    if (oldRegisters.state != Im2ColState::Done) {
        throw std::invalid_argument(
            "done transition requires old state ST_DONE");
    }
    ControlRegisters next = beginNext(oldRegisters);
    next.state = Im2ColState::Idle;
    next.done = true;
    return next;
}

CycleObservation
observe(uint64_t cycle, const ControlRegisters &registers)
{
    return {cycle, registers.state, busy(registers), registers.done};
}

} // namespace gem5::sau_n
