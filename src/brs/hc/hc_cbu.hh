#ifndef __BRS_HC_HC_CBU_HH__
#define __BRS_HC_HC_CBU_HH__

#include <cstdint>

#include "brs/hc/hc_protocol.hh"

namespace gem5
{
namespace brs
{

struct HcCbuIssue
{
    bool valid = false;
    HcRequest firstRequest;
    bool twoShot = false;
    uint64_t secondWriteData = 0;
};

struct HcCbuOutput
{
    bool ready = false;
    bool busy = false;
    bool complete = false;
    uint32_t result = 0;
    HcRequest request;
};

class HcCbu
{
  public:
    enum class State : uint8_t
    {
        Idle,
        SendFirst,
        WaitSecond
    };

    HcCbu();

    void reset();
    HcCbuOutput evaluate(const HcResponse &response) const;
    void clock(const HcCbuIssue &issue, const HcResponse &response);

    State state() const { return currentState; }
    bool busy() const { return currentState != State::Idle; }

  private:
    State currentState = State::Idle;
    HcRequest requestReg;
    uint64_t secondWriteDataReg = 0;
    bool twoShotReg = false;
};

} // namespace brs
} // namespace gem5

#endif // __BRS_HC_HC_CBU_HH__
