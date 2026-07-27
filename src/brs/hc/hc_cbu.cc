#include "brs/hc/hc_cbu.hh"

namespace gem5
{
namespace brs
{

HcCbu::HcCbu()
{
    reset();
}

void
HcCbu::reset()
{
    currentState = State::Idle;
    requestReg = {};
    secondWriteDataReg = 0;
    twoShotReg = false;
}

HcCbuOutput
HcCbu::evaluate(const HcResponse &response) const
{
    HcCbuOutput output;
    output.ready = currentState == State::Idle;
    output.busy = currentState != State::Idle;
    output.result = response.readData;
    if (output.busy) {
        output.request = requestReg;
    }

    const bool responseFire = output.busy && response.valid;
    output.complete = responseFire &&
        (currentState == State::WaitSecond ||
         (currentState == State::SendFirst && !twoShotReg));
    return output;
}

void
HcCbu::clock(const HcCbuIssue &issue, const HcResponse &response)
{
    switch (currentState) {
      case State::Idle:
        if (issue.valid && issue.firstRequest.hasTransaction()) {
            requestReg = issue.firstRequest;
            secondWriteDataReg = issue.secondWriteData;
            twoShotReg = issue.twoShot;
            currentState = State::SendFirst;
        }
        break;

      case State::SendFirst:
        if (response.valid) {
            if (twoShotReg) {
                requestReg.writeData = secondWriteDataReg;
                currentState = State::WaitSecond;
            } else {
                currentState = State::Idle;
            }
        }
        break;

      case State::WaitSecond:
        if (response.valid) {
            currentState = State::Idle;
        }
        break;
    }
}

} // namespace brs
} // namespace gem5
