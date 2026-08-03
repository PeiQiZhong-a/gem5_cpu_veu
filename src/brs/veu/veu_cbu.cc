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
    commonCbu.reset();
}

VeuCbuOutput
VeuCbu::evaluate(const VeuResponse &response) const
{
    return commonCbu.evaluate(response);
}

void
VeuCbu::clock(const VeuCbuIssue &issue, const VeuResponse &response)
{
    HcCbuIssue commonIssue;
    commonIssue.valid = issue.valid;
    commonIssue.firstRequest.csrAddr = issue.csrAddr;
    commonIssue.firstRequest.csrRead = issue.csrRead;
    commonIssue.firstRequest.csrWrite = issue.csrWrite;
    commonIssue.firstRequest.writeType =
        static_cast<uint8_t>(issue.writeType);
    commonIssue.firstRequest.writeData =
        packVeuOperands(issue.operand1, issue.operand2);
    commonIssue.firstRequest.veStart = issue.veStart;
    commonIssue.twoShot = isTwoShotVeuInstruction(issue.operation);
    commonIssue.secondWriteData =
        packVeuOperands(issue.operand3, issue.operand3);
    commonCbu.clock(commonIssue, response);
}

} // namespace brs
} // namespace gem5
