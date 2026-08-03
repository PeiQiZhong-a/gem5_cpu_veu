#ifndef __BRS_SAU_SAU_N_ENDPOINT_HH__
#define __BRS_SAU_SAU_N_ENDPOINT_HH__

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "brs/memory/dut_kui_memory_model.hh"
#include "brs/sau/sau_endpoint.hh"
#include "brs/sau/sau_n_config_adapter.hh"
#include "brs/sau/sau_n_memory_view.hh"
#include "sau_n/streaming_conv_pipeline_model.hh"

namespace gem5::brs
{

struct SauNEndpointTrace
{
    std::array<uint64_t, 4> payloads{};
    SauNResolvedConfig config;
    bool hasConfig = false;
    std::vector<uint8_t> stateHistory;
    std::string completionReason;
};

// CPU-facing endpoint for sau_n's local-scratchpad-backed streaming model.
// The endpoint owns the model and its non-owning memory view, while the
// DutKuiMemoryModel remains owned by PipelineMiniCPU.
class SauNEndpoint final : public SauEndpoint
{
  public:
    enum class State : uint8_t
    {
        Idle,
        Starting,
        Running,
        Finishing,
        Responding,
        Recovery,
    };

    SauNEndpoint(
        DutKuiMemoryModel &memory,
        uint32_t dataMemoryBase = 0x29120000,
        uint64_t dataMemorySize = 0x00030000);

    void reset() override;
    SauResponse evaluate() const override { return responseReg; }
    void clock(const SauRequest &request) override;
    SauMemoryOutput evaluateMemory() const override;
    void clockMemory(const SauMemoryResponse &response) override;
    void clockTick(
        const SauRequest &request,
        const SauMemoryResponse &memoryResponse) override;

    void setTraceEnabled(bool enabled);
    bool traceEnabled() const { return traceIsEnabled; }
    const SauNEndpointTrace &trace() const { return traceReg; }

    State state() const { return currentState; }
    const SauNConfigAdapter &configAdapter() const { return configState; }
    const SauNResolvedConfig *activeConfig() const
    {
        return configState.activeConfig();
    }
    uint64_t operationStartCount() const { return operationStarts; }
    uint64_t operationCompleteCount() const { return operationCompletes; }
    uint64_t modelTickCount() const { return modelTicks; }
    uint64_t endpointCycle() const { return cycle; }
    uint64_t roiStartCycle() const { return currentRoiStartCycle; }
    uint64_t roiEndCycle() const { return currentRoiEndCycle; }
    const sau_n::StreamingConvPipelineStats *streamingStats() const
    {
        return model ? &model->stats() : nullptr;
    }
    const sau_n::StreamingConvPipelineCycle *lastModelCycle() const
    {
        return lastModelCycleValid ? &lastCycle : nullptr;
    }
    const std::vector<int8_t> *modelOutputs() const
    {
        return model ? &model->outputs() : nullptr;
    }

  private:
    static SauInstruction setInstruction(uint8_t slot);
    static bool isMget4Lsb(const SauRequest &request);
    static bool sameRequest(
        const SauRequest &lhs, const SauRequest &rhs);

    void handleHcRequest(const SauRequest &request);
    void startOperation(const SauNResolvedConfig &config);
    void recordConfigTrace(const SauNResolvedConfig &config);
    std::string payloadSummary(
        const std::array<uint64_t, 4> &payloads) const;
    std::string configSummary(const SauNResolvedConfig &config) const;

    DutKuiMemoryModel &memory;
    uint32_t dataMemoryBase;
    uint64_t dataMemorySize;
    SauNConfigAdapter configState;
    std::unique_ptr<SauNMemoryView> memoryView;
    std::unique_ptr<sau_n::StreamingConvPipelineModel> model;

    State currentState = State::Idle;
    SauRequest activeRequest;
    SauResponse responseReg;
    SauMemoryResponse storedMemoryResponse;

    uint64_t cycle = 0;
    uint64_t operationStarts = 0;
    uint64_t operationCompletes = 0;
    uint64_t modelTicks = 0;
    uint64_t currentRoiStartCycle = 0;
    uint64_t currentRoiEndCycle = 0;

    bool traceIsEnabled = false;
    SauNEndpointTrace traceReg;
    bool lastModelCycleValid = false;
    sau_n::StreamingConvPipelineCycle lastCycle;
};

} // namespace gem5::brs

#endif // __BRS_SAU_SAU_N_ENDPOINT_HH__
