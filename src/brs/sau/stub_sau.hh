#ifndef __BRS_SAU_STUB_SAU_HH__
#define __BRS_SAU_STUB_SAU_HH__

#include <array>
#include <cstdint>

#include "brs/sau/sau_endpoint.hh"

namespace gem5
{
namespace brs
{

// Register-accurate SAU CSR stub for CPU-side bring-up. It intentionally
// models only the HC request/response path. Compute and SRAM traffic belong
// to a future RTL-derived SauEndpoint implementation.
class StubSau : public SauEndpoint
{
  public:
    enum class State : uint8_t
    {
        Idle,
        Waiting,
        Responding,
        Recovery
    };

    explicit StubSau(uint32_t responseLatencyCycles = 1);

    void reset() override;
    SauResponse evaluate() const override;
    void clock(const SauRequest &request) override;
    SauMemoryOutput evaluateMemory() const override { return {}; }
    void clockMemory(const SauMemoryResponse &) override {}

    void setResponseLatencyCycles(uint32_t cycles);

    State state() const { return currentState; }
    uint32_t responseLatencyCycles() const { return configuredLatency; }
    uint64_t acceptedRequestCount() const { return acceptedRequests; }
    uint64_t responseCount() const { return responses; }
    const SauRequest &lastAcceptedRequest() const { return requestReg; }
    uint64_t slotValue(uint8_t slot) const;

  private:
    void acceptRequest(const SauRequest &request);
    uint32_t readCsr(uint16_t address) const;
    void writeCsr(uint16_t address, uint64_t data);

    uint32_t configuredLatency = 1;
    State currentState = State::Idle;
    uint32_t remainingCycles = 0;
    uint32_t responseDataReg = 0;
    SauRequest requestReg;
    std::array<uint64_t, SauSlotCount> slots{};
    uint64_t acceptedRequests = 0;
    uint64_t responses = 0;
};

} // namespace brs
} // namespace gem5

#endif // __BRS_SAU_STUB_SAU_HH__
