#include "brs/veu/veu_cbu.hh"

namespace gem5
{
namespace brs
{

VeuCbu::VeuCbu()
{
    reset();
}

void
VeuCbu::reset()
{
    currentState = State::Idle;
    requestReg = {};
    operand3Reg = 0;
    twoShot = false;
}

VeuCbuOutput
VeuCbu::evaluate(const VeuResponse &response) const
{
    VeuCbuOutput output;
    output.ready = currentState == State::Idle;
    output.busy = currentState != State::Idle;
    output.result = response.readData;

    if (output.busy) {
        output.request = requestReg;
    }

    const bool responseFire = output.busy && response.valid;
    output.complete = responseFire &&
        ((currentState == State::SendFirst && !twoShot) ||
         currentState == State::WaitSecond);
    return output;
}

void
VeuCbu::clock(const VeuCbuIssue &issue, const VeuResponse &response)
{
    switch (currentState) {
      case State::Idle:
        if (issue.valid) {
            requestReg.csrAddr = issue.csrAddr;
            requestReg.csrRead = issue.csrRead;
            requestReg.csrWrite = issue.csrWrite;
            requestReg.writeType =
                static_cast<uint8_t>(issue.writeType);
            requestReg.writeData =
                packVeuOperands(issue.operand1, issue.operand2);
            requestReg.veStart = issue.veStart;
            operand3Reg = issue.operand3;
            twoShot = isTwoShotVeuInstruction(issue.operation);
            currentState = State::SendFirst;
        }
        break;

      case State::SendFirst:
        if (response.valid) {
            if (twoShot) {
                requestReg.writeData =
                    packVeuOperands(operand3Reg, operand3Reg);
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
