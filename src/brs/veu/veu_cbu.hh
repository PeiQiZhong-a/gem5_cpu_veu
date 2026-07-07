#ifndef __BRS_VEU_VEU_CBU_HH__
#define __BRS_VEU_VEU_CBU_HH__

#include <cstdint>

#include "brs/veu/veu_protocol.hh"

namespace gem5
{
namespace brs
{

struct VeuCbuIssue
{
    bool valid = false;
    VeuInstruction operation = VeuInstruction::Unknown;
    uint16_t csrAddr = 0;
    bool csrRead = false;
    bool csrWrite = false;
    VeuWriteType writeType = VeuWriteType::Write;
    uint32_t veStart = 0;
    uint32_t operand1 = 0;
    uint32_t operand2 = 0;
    uint32_t operand3 = 0;
};

struct VeuCbuOutput
{
    bool ready = false;
    bool busy = false;
    bool complete = false;
    uint32_t result = 0;
    VeuRequest request;
};

class VeuCbu
{
  public:
    enum class State : uint8_t
    {
        Idle,
        SendFirst,
        WaitSecond
    };

    VeuCbu();

    void reset();

    // Evaluate the signals driven during the current cycle. The response is
    // combinational input, matching Spirit CBU's resp_fire/complete logic.
    VeuCbuOutput evaluate(const VeuResponse &response) const;

    // Advance the CBU at one CPU clock edge. Call evaluate() before clock()
    // when recording the signals observed during that cycle.
    void clock(const VeuCbuIssue &issue, const VeuResponse &response);

    State state() const { return currentState; }
    bool busy() const { return currentState != State::Idle; }

  private:
    State currentState = State::Idle;
    VeuRequest requestReg;
    uint32_t operand3Reg = 0;
    bool twoShot = false;
};

} // namespace brs
} // namespace gem5

#endif // __BRS_VEU_VEU_CBU_HH__
