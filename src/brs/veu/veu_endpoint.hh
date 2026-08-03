#ifndef __BRS_VEU_VEU_ENDPOINT_HH__
#define __BRS_VEU_VEU_ENDPOINT_HH__

#include "brs/veu/veu_protocol.hh"

namespace gem5
{
namespace brs
{

// Cycle-level CPU-to-VEU boundary. During a cycle, evaluate() exposes the
// current hc2rv response. At the following clock edge, clock() samples the
// rv2hc request that the CBU drove during that cycle.
class VeuEndpoint
{
  public:
    virtual ~VeuEndpoint() = default;

    virtual void reset() = 0;
    virtual VeuResponse evaluate() const = 0;
    virtual void clock(const VeuRequest &request) = 0;

    // Optional TCM master and crossbar-lock sideband. FakeVEU does not access
    // memory, so defaults keep existing endpoints source-compatible.
    virtual VeuMemoryOutput evaluateMemory() const { return {}; }
    virtual void clockMemory(const VeuMemoryResponse &) {}

    virtual void clockTick(
        const VeuRequest &request,
        const VeuMemoryResponse &memoryResponse)
    {
        clock(request);
        clockMemory(memoryResponse);
    }
};

} // namespace brs
} // namespace gem5

#endif // __BRS_VEU_VEU_ENDPOINT_HH__
