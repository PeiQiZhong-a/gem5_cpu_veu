#ifndef __BRS_HC_HC_ROUTER_HH__
#define __BRS_HC_HC_ROUTER_HH__

#include <cstdint>

#include "brs/hc/hc_protocol.hh"
#include "brs/sau/sau_protocol.hh"

namespace gem5
{
namespace brs
{

enum class HcTarget : uint8_t
{
    None,
    Veu,
    Sau
};

struct HcRoutedRequests
{
    HcTarget target = HcTarget::None;
    HcRequest veu;
    HcRequest sau;
};

class HcRouter
{
  public:
    static constexpr HcTarget decode(uint16_t address)
    {
        return address >= 0x100 && address <= 0x107 ? HcTarget::Veu :
               address >= SauCsrBase &&
                   address < SauCsrBase + SauSlotCount * 2 ? HcTarget::Sau :
               HcTarget::None;
    }

    HcRoutedRequests routeRequest(const HcRequest &request) const;
    HcResponse routeResponse(
        const HcRequest &request,
        const HcResponse &veuResponse,
        const HcResponse &sauResponse) const;
};

} // namespace brs
} // namespace gem5

#endif // __BRS_HC_HC_ROUTER_HH__
