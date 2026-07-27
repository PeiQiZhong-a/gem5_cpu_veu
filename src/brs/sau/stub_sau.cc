#include "brs/sau/stub_sau.hh"

namespace gem5
{
namespace brs
{

StubSau::StubSau(uint32_t responseLatencyCycles)
{
    setResponseLatencyCycles(responseLatencyCycles);
    reset();
}

void
StubSau::reset()
{
    currentState = State::Idle;
    remainingCycles = 0;
    responseDataReg = 0;
    requestReg = {};
    slots.fill(0);
    acceptedRequests = 0;
    responses = 0;
}

SauResponse
StubSau::evaluate() const
{
    SauResponse response;
    response.valid = currentState == State::Responding;
    response.readData = response.valid ? responseDataReg : 0;
    return response;
}

void
StubSau::setResponseLatencyCycles(uint32_t cycles)
{
    configuredLatency = cycles == 0 ? 1 : cycles;
}

uint64_t
StubSau::slotValue(uint8_t slot) const
{
    return slot >= 1 && slot <= SauSlotCount ? slots[slot - 1] : 0;
}

uint32_t
StubSau::readCsr(uint16_t address) const
{
    if (!isSauCsr(address)) {
        return 0;
    }
    const uint16_t offset = address - SauCsrBase;
    const uint64_t value = slots[offset / 2];
    return (offset & 1) ?
        static_cast<uint32_t>(value >> 32) :
        static_cast<uint32_t>(value);
}

void
StubSau::writeCsr(uint16_t address, uint64_t data)
{
    if (!isSauCsr(address) || ((address - SauCsrBase) & 1) != 0) {
        return;
    }
    slots[(address - SauCsrBase) / 2] = data;
}

void
StubSau::acceptRequest(const SauRequest &request)
{
    requestReg = request;
    ++acceptedRequests;

    if (request.csrWrite) {
        writeCsr(request.csrAddr, request.writeData);
    }
    responseDataReg = request.csrRead ? readCsr(request.csrAddr) : 0;

    if (configuredLatency == 1) {
        currentState = State::Responding;
        remainingCycles = 0;
    } else {
        currentState = State::Waiting;
        remainingCycles = configuredLatency - 1;
    }
}

void
StubSau::clock(const SauRequest &request)
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
        ++responses;
        currentState = State::Recovery;
        break;

      case State::Recovery:
        // A level-held request is the transaction just acknowledged, not a
        // new transaction. Require an idle cycle before accepting another
        // request so one csr_valid pulse can never duplicate an MSET/MGET.
        if (!request.hasTransaction()) {
            currentState = State::Idle;
        }
        break;
    }
}

} // namespace brs
} // namespace gem5
