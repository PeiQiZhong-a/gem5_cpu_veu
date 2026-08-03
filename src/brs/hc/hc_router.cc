#include "brs/hc/hc_router.hh"

namespace gem5
{
namespace brs
{

HcRoutedRequests
HcRouter::routeRequest(const HcRequest &request) const
{
    HcRoutedRequests routed;
    if (!request.hasTransaction()) {
        return routed;
    }

    routed.target = decode(request.csrAddr);
    if (routed.target == HcTarget::Veu) {
        routed.veu = request;
    } else if (routed.target == HcTarget::Sau) {
        routed.sau = request;
    }
    return routed;
}

HcResponse
HcRouter::routeResponse(
    const HcRequest &request,
    const HcResponse &veuResponse,
    const HcResponse &sauResponse) const
{
    if (!request.hasTransaction()) {
        return {};
    }

    switch (decode(request.csrAddr)) {
      case HcTarget::Veu:
        return veuResponse;
      case HcTarget::Sau:
        return sauResponse;
      case HcTarget::None:
      default:
        return {};
    }
}

} // namespace brs
} // namespace gem5
