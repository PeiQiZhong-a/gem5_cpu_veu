#ifndef __BRS_SAU_SAU_N_CONFIG_ADAPTER_HH__
#define __BRS_SAU_SAU_N_CONFIG_ADAPTER_HH__

#include <array>
#include <cstdint>
#include <string>

#include "brs/sau/conv3_csr_config.hh"
#include "sau_n/sau_types.hh"

namespace gem5::brs
{

struct SauNResolvedConfig
{
    Conv3Config abi;
    sau_n::PipelineResolvedConfig pipeline;
    sau_n::PipelineDerivedConfig derived;
    std::array<uint64_t, 4> payloads{};
};

enum class SauNConfigError : uint8_t
{
    None,
    AbiValidation,
    StreamingValidation,
};

const char *sauNConfigErrorString(SauNConfigError error);

struct SauNDecodeResult
{
    bool valid = false;
    SauNResolvedConfig config;
    std::array<uint64_t, 4> payloads{};
    SauNConfigError error = SauNConfigError::None;
    Conv3ConfigError abiError = Conv3ConfigError::None;
    std::string detail;
};

struct SauNWriteResult
{
    bool accepted = false;
    bool committed = false;
    SauNConfigError error = SauNConfigError::None;
    Conv3ConfigError abiError = Conv3ConfigError::None;
    std::string detail;
};

// Adapts the frozen four-payload Conv3 ABI to sau_n's resolved streaming
// configuration.  The ABI shadow/active transaction remains owned by
// Conv3CsrConfig; this class adds the sau_n validation before Set4 commits.
class SauNConfigAdapter
{
  public:
    using Payloads = Conv3CsrConfig::Payloads;

    SauNConfigAdapter(
        uint32_t dataMemoryBase = 0x29120000,
        uint64_t dataMemorySize = 0x00030000);

    void reset();

    SauNWriteResult write(SauInstruction operation, uint64_t payload);
    bool completeActiveOperation();

    static SauNDecodeResult decode(
        const Payloads &payloads,
        uint32_t dataMemoryBase,
        uint64_t dataMemorySize);

    bool busy() const { return csrState.busy(); }
    bool hasActiveConfig() const { return activeValid; }
    const SauNResolvedConfig *activeConfig() const
    {
        return activeValid ? &active : nullptr;
    }
    const Conv3CsrConfig &csrConfig() const { return csrState; }

  private:
    static sau_n::PipelineResolvedConfig makePipelineConfig(
        const Conv3Config &abi);
    static SauNWriteResult fromWireResult(
        const Conv3WriteResult &result);
    static SauNWriteResult fromDecodeResult(
        const SauNDecodeResult &result);

    Conv3CsrConfig csrState;
    SauNResolvedConfig active;
    bool activeValid = false;
};

} // namespace gem5::brs

#endif // __BRS_SAU_SAU_N_CONFIG_ADAPTER_HH__
