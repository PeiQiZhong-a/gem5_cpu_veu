#include "sau_n/sau_types.hh"

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

bool
supportedWeightGenerator(std::string_view generator)
{
    return generator == "tb_weight_value_v1" || generator == "zero" ||
        generator == "ones";
}

bool
supportedBiasGenerator(std::string_view generator)
{
    return generator == "tb_bias_value_v1" || generator == "zero";
}

} // anonymous namespace

std::string_view
pipelineStateName(PipelineState state)
{
    switch (state) {
      case PipelineState::Idle:
        return "IDLE";
      case PipelineState::CollectTile:
        return "COLLECT_TILE";
      case PipelineState::LaunchSa:
        return "LAUNCH_SA";
      case PipelineState::StreamK:
        return "STREAM_K";
      case PipelineState::WaitResult:
        return "WAIT_RESULT";
      case PipelineState::DrainOutput:
        return "DRAIN_OUTPUT";
      case PipelineState::Done:
        return "DONE";
    }
    throw std::invalid_argument("invalid pipeline state value");
}

PipelineDerivedConfig
validateAndDerive(const PipelineResolvedConfig &config)
{
    if (config.schemaVersion != SchemaVersion) {
        throw std::invalid_argument("schema_version must be 1");
    }
    if (config.name.empty()) {
        throw std::invalid_argument("name must be a non-empty string");
    }

    DerivedConfig im2col;
    try {
        im2col = validateAndDerive(config.im2col);
    } catch (const std::invalid_argument &error) {
        throw std::invalid_argument(std::string("im2col.") + error.what());
    }
    if (config.im2col.kernelH != SauKernelHeight ||
        config.im2col.kernelW != SauKernelWidth) {
        throw std::invalid_argument(
            "im2col kernel_h and kernel_w must both be 3");
    }
    requireRange(config.im2col.c, 1, SauMaxChannels, "im2col.c");
    requireRange(config.outChannels, 1, SauMaxOutChannels, "out_channels");
    requireRange(config.cutbit, 0, SauMaxCutbit, "cutbit");
    if (!supportedWeightGenerator(config.weightGenerator)) {
        throw std::invalid_argument("unsupported weight_generator");
    }
    if (!supportedBiasGenerator(config.biasGenerator)) {
        throw std::invalid_argument("unsupported bias_generator");
    }

    PipelineDerivedConfig derived;
    derived.im2col = im2col;
    derived.k = checkedMultiply(
        config.im2col.c, SauKernelHeight * SauKernelWidth, "pipeline K");
    derived.expectedTiles = checkedMultiply(
        checkedMultiply(
            config.im2col.n, im2col.hGroups, "expected tile count"),
        im2col.wGroups, "expected tile count");
    const uint64_t expectedVectors = checkedMultiply(
        derived.expectedTiles, derived.k, "expected vector count");
    if (expectedVectors != im2col.expectedVectors) {
        throw std::invalid_argument(
            "pipeline tile derivation disagrees with Im2Col expected_vectors");
    }

    derived.expectedOutputs = checkedMultiply(
        checkedMultiply(
            checkedMultiply(
                config.im2col.n, config.im2col.outH,
                "expected output count"),
            config.im2col.outW, "expected output count"),
        config.outChannels, "expected output count");
    derived.expectedMacs = checkedMultiply(
        derived.expectedOutputs, derived.k, "expected useful MAC count");
    return derived;
}

void
validateOutputReady(const OutputReadyConfig &config)
{
    if (config.period == 0) {
        throw std::invalid_argument("output ready period must be >= 1");
    }
    if (config.highCycles == 0 || config.highCycles > config.period) {
        throw std::invalid_argument(
            "output ready high_cycles must be in [1, period]");
    }
}

bool
outputReady(uint64_t cycle, const OutputReadyConfig &config)
{
    validateOutputReady(config);
    return cycle % config.period < config.highCycles;
}

uint64_t
peIndex(uint64_t row, uint64_t column)
{
    requireRange(row, 0, SauRows - 1, "PE row");
    requireRange(column, 0, SauColumns - 1, "PE column");
    return row * SauColumns + column;
}

bool
pipelineDrained(
    const PipelineDrainStatus &status,
    const PipelineDerivedConfig &derived)
{
    return !status.im2colCanFeed && status.im2colFifoEmpty &&
        status.tileBufferEmpty && status.sauIdle &&
        status.completedTiles == derived.expectedTiles &&
        status.writtenOutputs == derived.expectedOutputs &&
        !status.outputPending;
}

} // namespace gem5::sau_n
