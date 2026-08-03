#include "brs/sau/sau_cbu.hh"

namespace gem5
{
namespace brs
{

SauCbu::SauCbu()
{
    reset();
}

void
SauCbu::reset()
{
    commonCbu.reset();
}

SauCbuOutput
SauCbu::evaluate(const SauResponse &response) const
{
    return commonCbu.evaluate(response);
}

void
SauCbu::clock(const SauCbuIssue &issue, const SauResponse &response)
{
    HcCbuIssue commonIssue;
    commonIssue.valid = issue.valid;
    commonIssue.firstRequest.csrAddr = issue.csrAddr;
    commonIssue.firstRequest.csrRead = issue.csrRead;
    commonIssue.firstRequest.csrWrite = issue.csrWrite;
    commonIssue.firstRequest.writeType =
        static_cast<uint8_t>(issue.writeType);
    commonIssue.firstRequest.writeData =
        packSauOperands(issue.operand1, issue.operand2);
    commonIssue.firstRequest.veStart = issue.veStart;
    commonCbu.clock(commonIssue, response);
}

} // namespace brs
} // namespace gem5
