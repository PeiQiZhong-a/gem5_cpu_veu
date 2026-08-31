#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>

#include "sau_n/banked_scratchpad.hh"
#include "sau_n/im2col_address.hh"

namespace gem5::sau_n
{
namespace
{

class ArrayScratchpadBacking final : public ScratchpadBacking
{
  public:
    uint8_t read(uint64_t bank, uint64_t row) const override
    {
        return storage[bank][row];
    }

    void write(uint64_t bank, uint64_t row, uint8_t value) override
    {
        storage[bank][row] = value;
    }

    std::array<std::array<uint8_t, ScratchpadRows>, ScratchpadBanks> storage{};
};

ResolvedConfig
baseConfig()
{
    ResolvedConfig config;
    config.name = "scratchpad";
    config.n = 1;
    config.c = 1;
    config.h = 1;
    config.w = 1;
    config.outH = 1;
    config.outW = 1;
    config.kernelH = 1;
    config.kernelW = 1;
    config.strideH = 1;
    config.strideW = 1;
    config.dilationH = 1;
    config.dilationW = 1;
    return config;
}

void
expectPreloadedTensor(
    const BankedScratchpad &scratchpad, const ResolvedConfig &config)
{
    const ChwAddressMapper mapper(config);
    for (uint64_t n = 0; n < config.n; ++n) {
        for (uint64_t c = 0; c < config.c; ++c) {
            for (uint64_t h = 0; h < config.h; ++h) {
                for (uint64_t w = 0; w < config.w; ++w) {
                    const auto location = mapper.locate(n, c, h, w);
                    EXPECT_EQ(scratchpad.read(location.bank, location.row),
                              tbActValueV1(n, c, h, w));
                }
            }
        }
    }
}

TEST(BankedScratchpad, SupportsClearReadWriteAndBounds)
{
    BankedScratchpad scratchpad;
    EXPECT_EQ(scratchpad.read(3, 9), uint8_t{0});

    scratchpad.write(3, 9, 0xab);
    EXPECT_EQ(scratchpad.read(3, 9), uint8_t{0xab});
    scratchpad.clear();
    EXPECT_EQ(scratchpad.read(3, 9), uint8_t{0});

    EXPECT_THROW(scratchpad.read(16, 0), std::out_of_range);
    EXPECT_THROW(scratchpad.read(0, 4096), std::out_of_range);
    EXPECT_THROW(scratchpad.write(16, 0, 0), std::out_of_range);
    EXPECT_THROW(scratchpad.write(0, 4096, 0), std::out_of_range);
}

TEST(BankedScratchpad, PreloadsPackedW5AndLeavesUnusedLanesZero)
{
    auto config = baseConfig();
    config.n = 2;
    config.c = 2;
    config.h = 4;
    config.w = 5;
    config.outH = 4;
    config.outW = 5;
    config.spadBase = 11;
    BankedScratchpad scratchpad;
    scratchpad.write(15, 11, 0xff);

    scratchpad.preload(config);

    expectPreloadedTensor(scratchpad, config);
    EXPECT_EQ(scratchpad.read(15, 11), uint8_t{0});
    EXPECT_EQ(scratchpad.read(5, 12), uint8_t{0});
}

TEST(BankedScratchpad, PreloadsSplitW20AcrossTwoWordsPerHRow)
{
    auto config = baseConfig();
    config.n = 2;
    config.c = 2;
    config.h = 3;
    config.w = 20;
    config.outH = 3;
    config.outW = 20;
    config.spadBase = 5;
    BankedScratchpad scratchpad;

    scratchpad.preload(config);

    expectPreloadedTensor(scratchpad, config);
    const ChwAddressMapper mapper(config);
    const auto w0 = mapper.locate(0, 0, 0, 0);
    const auto w16 = mapper.locate(0, 0, 0, 16);
    EXPECT_EQ(w0.bank, w16.bank);
    EXPECT_NE(w0.row, w16.row);
    EXPECT_EQ(scratchpad.read(w0.bank, w0.row), uint8_t{1});
    EXPECT_EQ(scratchpad.read(w16.bank, w16.row), uint8_t{17});
}

TEST(BankedScratchpad, SupportsFootprintEndingAtLastRow)
{
    auto config = baseConfig();
    config.spadBase = 4095;
    BankedScratchpad scratchpad;

    EXPECT_NO_THROW(scratchpad.preload(config));
    EXPECT_EQ(scratchpad.read(0, 4095), uint8_t{1});

    config.h = 17;
    EXPECT_THROW(scratchpad.preload(config), std::invalid_argument);
}

TEST(BankedScratchpad, ProducesCombinationalResponseForValidBanks)
{
    BankedScratchpad scratchpad;
    scratchpad.write(0, 3, 0xaa);
    scratchpad.write(7, 9, 0xbb);
    SramRequest request;
    request.valid[0] = true;
    request.address[0] = 3;
    request.valid[7] = true;
    request.address[7] = 9;
    request.address[1] = 4096;

    const auto response = scratchpad.combinationalResponse(request);

    EXPECT_TRUE(response.valid[0]);
    EXPECT_EQ(response.data[0], uint8_t{0xaa});
    EXPECT_TRUE(response.valid[7]);
    EXPECT_EQ(response.data[7], uint8_t{0xbb});
    EXPECT_FALSE(response.valid[1]);
    EXPECT_EQ(response.data[1], uint8_t{0});
}

TEST(BankedScratchpad, RejectsOutOfRangeValidRequest)
{
    BankedScratchpad scratchpad;
    SramRequest request;
    request.valid[0] = true;
    request.address[0] = 4096;

    EXPECT_THROW(
        scratchpad.combinationalResponse(request), std::out_of_range);
}

TEST(BankedScratchpad, DelegatesExternalBackingWithoutGeneratorOrClear)
{
    ArrayScratchpadBacking backing;
    backing.storage[4][17] = 0xa5;
    BankedScratchpad scratchpad(backing);

    EXPECT_TRUE(scratchpad.usesExternalBacking());
    EXPECT_EQ(scratchpad.read(4, 17), uint8_t{0xa5});
    scratchpad.write(4, 17, 0x5a);
    EXPECT_EQ(backing.storage[4][17], uint8_t{0x5a});

    EXPECT_THROW(scratchpad.clear(), std::logic_error);
    EXPECT_THROW(scratchpad.preload(baseConfig()), std::logic_error);
}

} // anonymous namespace
} // namespace gem5::sau_n
