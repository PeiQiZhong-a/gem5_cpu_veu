#include "brs/veu/fake_veu.hh"

namespace gem5
{
namespace brs
{

FakeVeu::FakeVeu(
    uint32_t responseLatencyCycles, uint32_t responseData)
  : configuredResponseData(responseData)
{
    setResponseLatencyCycles(responseLatencyCycles);
    reset();
}

void
FakeVeu::reset()
{
    currentState = State::Idle;
    remainingCycles = 0;
    requestReg = {};
    acceptedRequests = 0;
    responses = 0;
}

VeuResponse
FakeVeu::evaluate() const
{
    VeuResponse response;
    response.valid = currentState == State::Responding;
    response.readData = response.valid ? configuredResponseData : 0;
    return response;
}

void
FakeVeu::setResponseLatencyCycles(uint32_t cycles)
{
    // A synchronous endpoint cannot respond before the first edge that
    // samples a request. Treat zero as the minimum one-cycle latency.
    configuredLatency = cycles == 0 ? 1 : cycles;
}

void
FakeVeu::acceptRequest(const VeuRequest &request)
{
    requestReg = request;
    ++acceptedRequests;

    if (configuredLatency == 1) {
        currentState = State::Responding;
        remainingCycles = 0;
    } else {
        currentState = State::Waiting;
        remainingCycles = configuredLatency - 1;
    }
}

void
FakeVeu::clock(const VeuRequest &request)
{
    switch (currentState) {
      case State::Idle:
        if (request.hasTransaction()) {
            acceptRequest(request);
        }
        break;

      case State::Waiting:
        if (remainingCycles > 1) {
            --remainingCycles;
        } else {
            remainingCycles = 0;
            currentState = State::Responding;
        }
        break;

      case State::Responding:
        // csr_valid is a one-cycle pulse. Do not sample the request on the
        // same edge: the CBU still drives the acknowledged first request
        // until that edge updates its own state.
        ++responses;
        currentState = State::Recovery;
        break;

      case State::Recovery:
        // One cycle after csr_valid, a still-active request is a new CBU
        // phase (the second VMADD/VMSUB transaction). A completed one-shot
        // CBU has dropped csr_re/csr_we by this point.
        if (request.hasTransaction()) {
            acceptRequest(request);
        } else {
            currentState = State::Idle;
        }
        break;
    }
}

} // namespace brs
} // namespace gem5
