#ifndef __BRS_PIPELINE_VEU_ISSUE_HH__
#define __BRS_PIPELINE_VEU_ISSUE_HH__

#include "brs/pipeline/pipeline_regs.hh"
#include "brs/hc/hc_cbu.hh"
#include "brs/veu/veu_cbu.hh"

namespace gem5
{

inline brs::VeuCbuIssue
makeVeuCbuIssue(
    const IDEX &decoded,
    uint32_t operand1,
    uint32_t operand2,
    uint32_t operand3)
{
    brs::VeuCbuIssue issue;
    issue.valid = decoded.valid && decoded.kind == InstrKind::VEU;
    issue.operation = decoded.veu_operation;
    issue.csrAddr = decoded.veu_csr_addr;
    issue.csrRead = decoded.veu_csr_read;
    issue.csrWrite = decoded.veu_csr_write;
    issue.writeType = decoded.veu_write_type;
    issue.veStart = decoded.veu_start;
    issue.operand1 = operand1;
    issue.operand2 = operand2;
    issue.operand3 = operand3;
    return issue;
}

inline brs::HcCbuIssue
makeVeuHcCbuIssue(
    const IDEX &decoded,
    uint32_t operand1,
    uint32_t operand2,
    uint32_t operand3)
{
    const brs::VeuCbuIssue veu = makeVeuCbuIssue(
        decoded, operand1, operand2, operand3);
    brs::HcCbuIssue issue;
    issue.valid = veu.valid;
    issue.firstRequest.csrAddr = veu.csrAddr;
    issue.firstRequest.csrRead = veu.csrRead;
    issue.firstRequest.csrWrite = veu.csrWrite;
    issue.firstRequest.writeType = static_cast<uint8_t>(veu.writeType);
    issue.firstRequest.writeData =
        brs::packHcOperands(veu.operand1, veu.operand2);
    issue.firstRequest.veStart = veu.veStart;
    issue.twoShot = brs::isTwoShotVeuInstruction(veu.operation);
    issue.secondWriteData =
        brs::packHcOperands(veu.operand3, veu.operand3);
    return issue;
}

} // namespace gem5

#endif // __BRS_PIPELINE_VEU_ISSUE_HH__
