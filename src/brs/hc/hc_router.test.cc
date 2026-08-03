#include "brs/hc/hc_router.hh"

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

TEST(HcRouterTest, RoutesVeuAndSauRangesMutuallyExclusively)
{
    HcRouter router;
    HcRequest request;
    request.csrWrite = true;

    request.csrAddr = 0x107;
    auto routed = router.routeRequest(request);
    EXPECT_EQ(routed.target, HcTarget::Veu);
    EXPECT_TRUE(routed.veu.hasTransaction());
    EXPECT_FALSE(routed.sau.hasTransaction());

    request.csrAddr = 0x200;
    routed = router.routeRequest(request);
    EXPECT_EQ(routed.target, HcTarget::Sau);
    EXPECT_FALSE(routed.veu.hasTransaction());
    EXPECT_TRUE(routed.sau.hasTransaction());

    request.csrAddr = 0x207;
    routed = router.routeRequest(request);
    EXPECT_EQ(routed.target, HcTarget::Sau);

    request.csrAddr = 0x208;
    routed = router.routeRequest(request);
    EXPECT_EQ(routed.target, HcTarget::None);
}

TEST(HcRouterTest, InvalidAndIdleRequestsReachNoEndpoint)
{
    HcRouter router;
    HcRequest request;
    request.csrAddr = 0x108;
    request.csrRead = true;
    auto routed = router.routeRequest(request);
    EXPECT_EQ(routed.target, HcTarget::None);
    EXPECT_FALSE(routed.veu.hasTransaction());
    EXPECT_FALSE(routed.sau.hasTransaction());

    request = {};
    request.csrAddr = 0x200;
    routed = router.routeRequest(request);
    EXPECT_EQ(routed.target, HcTarget::None);
}

TEST(HcRouterTest, OnlySelectedResponseCanCompleteCpuRequest)
{
    HcRouter router;
    HcRequest request;
    request.csrAddr = 0x101;
    request.csrRead = true;

    const HcResponse veu{false, 0x11111111};
    const HcResponse sau{true, 0x22222222};
    auto response = router.routeResponse(request, veu, sau);
    EXPECT_FALSE(response.valid);
    EXPECT_EQ(response.readData, veu.readData);

    request.csrAddr = 0x204;
    response = router.routeResponse(request, veu, sau);
    EXPECT_TRUE(response.valid);
    EXPECT_EQ(response.readData, sau.readData);
}

} // namespace
} // namespace brs
} // namespace gem5
