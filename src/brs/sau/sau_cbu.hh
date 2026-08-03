#ifndef __BRS_SAU_SAU_CBU_HH__
#define __BRS_SAU_SAU_CBU_HH__

#include <cstdint>

#include "brs/hc/hc_cbu.hh"
#include "brs/sau/sau_protocol.hh"

namespace gem5
{
namespace brs
{

struct SauCbuIssue
{
    bool valid = false;
    SauInstruction operation = SauInstruction::Unknown;
    uint16_t csrAddr = 0;
    bool csrRead = false;
    bool csrWrite = false;
    SauWriteType writeType = SauWriteType::Write;
    uint32_t veStart = 0;
    uint32_t operand1 = 0;
    uint32_t operand2 = 0;
};

using SauCbuOutput = HcCbuOutput;

class SauCbu
{
  public:
    using State = HcCbu::State;

    SauCbu();

    void reset();
    SauCbuOutput evaluate(const SauResponse &response) const;
    void clock(const SauCbuIssue &issue, const SauResponse &response);

    State state() const { return commonCbu.state(); }
    bool busy() const { return commonCbu.busy(); }

  private:
    HcCbu commonCbu;
};

} // namespace brs
} // namespace gem5

#endif // __BRS_SAU_SAU_CBU_HH__
