#include "brs/sau/sau_n_config_adapter.hh"

#include <exception>

#include "sau_n/im2col_types.hh"
#include "sau_n/streaming_pipeline_contract.hh"

namespace gem5::brs
{
namespace
{

SauNDecodeResult
abiFailure(
    const Conv3DecodeResult &decoded,
    const Conv3CsrConfig::Payloads &payloads)
{
    SauNDecodeResult result;
    result.payloads = payloads;
    result.error = SauNConfigError::AbiValidation;
    result.abiError = decoded.error;
    result.detail = conv3ConfigErrorString(decoded.error);
    return result;
}

} // anonymous namespace

const char *
sauNConfigErrorString(SauNConfigError error)
{
    switch (error) {
      case SauNConfigError::None:
        return "none";
      case SauNConfigError::AbiValidation:
        return "four-payload ABI validation failed";
      case SauNConfigError::StreamingValidation:
        return "sau_n streaming configuration validation failed";
    }
    return "unknown sau_n configuration error";
}

sau_n::PipelineResolvedConfig
SauNConfigAdapter::makePipelineConfig(const Conv3Config &abi)
{
    sau_n::PipelineResolvedConfig pipeline;
    pipeline.name = "sau_n_conv3_abi_v1";
    pipeline.im2col.name = "sau_n_conv3_abi_v1_im2col";
    pipeline.im2col.n = abi.batchN;
    pipeline.im2col.c = abi.inputC;
    pipeline.im2col.h = abi.inputH;
    pipeline.im2col.w = abi.inputW;
    pipeline.im2col.outH = abi.outputH;
    pipeline.im2col.outW = abi.outputW;
    pipeline.im2col.kernelH = sau_n::SauKernelHeight;
    pipeline.im2col.kernelW = sau_n::SauKernelWidth;
    pipeline.im2col.strideH = abi.strideMinus1 + 1;
    pipeline.im2col.strideW = abi.strideMinus1 + 1;
    pipeline.im2col.dilationH = 1;
    pipeline.im2col.dilationW = 1;
    pipeline.im2col.padTop = abi.padding;
    pipeline.im2col.padLeft = abi.padding;
    pipeline.im2col.spadBase = 0;
    pipeline.im2col.cfgDwMode = 0;
    pipeline.im2col.cfgKernelPattern = sau_n::KernelPatternAll;
    pipeline.im2col.inputGenerator = "tb_act_value_v1";
    pipeline.outChannels = abi.outputC;
    pipeline.cutbit = abi.cutbit;
    pipeline.weightGenerator = "tb_weight_value_v1";
    pipeline.biasGenerator = "tb_bias_value_v1";
    return pipeline;
}

SauNDecodeResult
SauNConfigAdapter::decode(
    const Payloads &payloads,
    uint32_t dataMemoryBase,
    uint64_t dataMemorySize)
{
    const Conv3DecodeResult abi = Conv3CsrConfig::decode(
        payloads, dataMemoryBase, dataMemorySize);
    if (!abi.valid) {
        return abiFailure(abi, payloads);
    }

    SauNDecodeResult result;
    result.payloads = payloads;
    result.config.abi = abi.config;
    result.config.payloads = payloads;
    result.config.pipeline = makePipelineConfig(abi.config);
    try {
        result.config.derived = sau_n::validateStreamingConfig(
            result.config.pipeline);
        result.config.pipeline.sharedSpad = result.config.derived.sharedSpad;
    } catch (const std::exception &error) {
        result.error = SauNConfigError::StreamingValidation;
        result.detail = error.what();
        return result;
    }

    result.valid = true;
    return result;
}

SauNConfigAdapter::SauNConfigAdapter(
    uint32_t dataMemoryBase, uint64_t dataMemorySize)
  : csrState(dataMemoryBase, dataMemorySize)
{
    reset();
}

void
SauNConfigAdapter::reset()
{
    csrState.reset();
    active = {};
    activeValid = false;
}

SauNWriteResult
SauNConfigAdapter::fromWireResult(const Conv3WriteResult &result)
{
    SauNWriteResult adapted;
    adapted.accepted = result.accepted;
    adapted.committed = result.committed;
    if (!result.accepted) {
        adapted.error = SauNConfigError::AbiValidation;
        adapted.abiError = result.error;
        adapted.detail = conv3ConfigErrorString(result.error);
    }
    return adapted;
}

SauNWriteResult
SauNConfigAdapter::fromDecodeResult(const SauNDecodeResult &result)
{
    SauNWriteResult adapted;
    adapted.error = result.error;
    adapted.abiError = result.abiError;
    adapted.detail = result.detail;
    return adapted;
}

SauNWriteResult
SauNConfigAdapter::write(SauInstruction operation, uint64_t payload)
{
    const bool isCommit = isSauSet(operation) && sauSlot(operation) == 4;
    if (!isCommit || csrState.busy() || csrState.shadowValidMask() != 0x7) {
        return fromWireResult(csrState.write(operation, payload));
    }

    Payloads candidate = csrState.shadowWords();
    candidate[3] = payload;
    const SauNDecodeResult decoded = decode(
        candidate, csrState.dataMemoryBase(), csrState.dataMemorySize());
    if (!decoded.valid) {
        return fromDecodeResult(decoded);
    }

    const Conv3WriteResult committed = csrState.write(operation, payload);
    const SauNWriteResult result = fromWireResult(committed);
    if (!result.committed) {
        return result;
    }
    active = decoded.config;
    activeValid = true;
    return result;
}

bool
SauNConfigAdapter::completeActiveOperation()
{
    return csrState.completeActiveOperation();
}

} // namespace gem5::brs
