#ifndef __BRS_SAU_SAU_ENDPOINT_HH__
#define __BRS_SAU_SAU_ENDPOINT_HH__

#include "brs/memory/sram_256_protocol.hh"
#include "brs/sau/sau_protocol.hh"

namespace gem5
{
namespace brs
{

struct SauMemoryOutput
{
    Sram256Request request;
    bool crossbarStart = false;
    bool crossbarDone = false;
};

using SauMemoryResponse = Sram256Response;

// Frozen CPU-side boundary for a cycle-level SAU gem5 model.
//
// evaluate*() exposes signals for the current tick without mutating state.
// clock*() samples those signals and advances exactly one clock edge.
// reset() must cancel all in-flight work and make the next evaluate*() idle.
class SauEndpoint
{
  public:
    virtual ~SauEndpoint() = default;

    virtual void reset() = 0;
    virtual SauResponse evaluate() const = 0;
    virtual void clock(const SauRequest &request) = 0;

    virtual SauMemoryOutput evaluateMemory() const = 0;
    virtual void clockMemory(const SauMemoryResponse &response) = 0;

    // Atomic one-edge hook used by the unified tick engine. Endpoints whose
    // HC and SRAM paths share state should override this method. The default
    // preserves source compatibility with v1.0 endpoint implementations.
    virtual void clockTick(
        const SauRequest &request,
        const SauMemoryResponse &memoryResponse)
    {
        clock(request);
        clockMemory(memoryResponse);
    }
};

} // namespace brs
} // namespace gem5

#endif // __BRS_SAU_SAU_ENDPOINT_HH__
