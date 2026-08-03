#ifndef __BRS_PIPELINE_SAU_ISSUE_HH__
#define __BRS_PIPELINE_SAU_ISSUE_HH__

#include "brs/pipeline/pipeline_regs.hh"
#include "brs/hc/hc_cbu.hh"
#include "brs/sau/sau_cbu.hh"

namespace gem5
{

inline brs::SauCbuIssue
makeSauCbuIssue(
    const IDEX &decoded,
    uint32_t operand1,
    uint32_t operand2)
{
    brs::SauCbuIssue issue;
    issue.valid = decoded.valid && decoded.kind == InstrKind::SAU;
    issue.operation = decoded.sau_operation;
    issue.csrAddr = decoded.sau_csr_addr;
    issue.csrRead = decoded.sau_csr_read;
    issue.csrWrite = decoded.sau_csr_write;
    issue.writeType = decoded.sau_write_type;
    issue.veStart = 0;
    issue.operand1 = operand1;
    issue.operand2 = operand2;
    return issue;
}

inline brs::HcCbuIssue
makeSauHcCbuIssue(
    const IDEX &decoded,
    uint32_t operand1,
    uint32_t operand2)
{
    const brs::SauCbuIssue sau =
        makeSauCbuIssue(decoded, operand1, operand2);
    brs::HcCbuIssue issue;
    issue.valid = sau.valid;
    issue.firstRequest.csrAddr = sau.csrAddr;
    issue.firstRequest.csrRead = sau.csrRead;
    issue.firstRequest.csrWrite = sau.csrWrite;
    issue.firstRequest.writeType = static_cast<uint8_t>(sau.writeType);
    issue.firstRequest.writeData =
        brs::packHcOperands(sau.operand1, sau.operand2);
    issue.firstRequest.veStart = sau.veStart;
    return issue;
}

} // namespace gem5

#endif // __BRS_PIPELINE_SAU_ISSUE_HH__
