#ifndef __BRS_VEU_FAKE_VEU_HH__
#define __BRS_VEU_FAKE_VEU_HH__

#include <cstdint>

#include "brs/veu/veu_endpoint.hh"

namespace gem5
{
namespace brs
{

class FakeVeu : public VeuEndpoint
{
  public:
    enum class State : uint8_t
    {
        Idle,
        Waiting,
        Responding,
        Recovery
    };

    explicit FakeVeu(
        uint32_t responseLatencyCycles = 1,
        uint32_t responseData = 0);

    void reset() override;
    VeuResponse evaluate() const override;
    void clock(const VeuRequest &request) override;

    void setResponseLatencyCycles(uint32_t cycles);
    void setResponseData(uint32_t data) { configuredResponseData = data; }

    uint32_t responseLatencyCycles() const { return configuredLatency; }
    uint32_t responseData() const { return configuredResponseData; }
    State state() const { return currentState; }
    uint64_t acceptedRequestCount() const { return acceptedRequests; }
    uint64_t responseCount() const { return responses; }
    const VeuRequest &lastAcceptedRequest() const { return requestReg; }

  private:
    void acceptRequest(const VeuRequest &request);

    uint32_t configuredLatency = 1;
    uint32_t configuredResponseData = 0;
    State currentState = State::Idle;
    uint32_t remainingCycles = 0;
    VeuRequest requestReg;
    uint64_t acceptedRequests = 0;
    uint64_t responses = 0;
};

} // namespace brs
} // namespace gem5

#endif // __BRS_VEU_FAKE_VEU_HH__
