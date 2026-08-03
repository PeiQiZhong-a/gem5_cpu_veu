#include <gtest/gtest.h>

#include <stdexcept>

#include "sau_n/im2col_address.hh"

namespace gem5::sau_n
{
namespace
{

ResolvedConfig
baseConfig()
{
    ResolvedConfig config;
    config.name = "address";
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

TEST(ChwAddressMapper, PacksThreeW5RowsPerScratchpadRow)
{
    auto config = baseConfig();
    config.n = 2;
    config.c = 2;
    config.h = 4;
    config.w = 5;
    config.outH = 4;
    config.outW = 5;
    config.spadBase = 7;
    const ChwAddressMapper mapper(config);

    EXPECT_EQ(mapper.derived().rowsPerWord, uint64_t{3});
    EXPECT_EQ(mapper.derived().spatialWordsPerChannel, uint64_t{2});
    EXPECT_EQ(mapper.locate(0, 0, 0, 0),
              (ChwAddress{0, 7, 0, 0}));
    EXPECT_EQ(mapper.locate(0, 0, 1, 2),
              (ChwAddress{0, 7, 7, 7}));
    EXPECT_EQ(mapper.locate(0, 0, 2, 4),
              (ChwAddress{0, 7, 14, 14}));
    EXPECT_EQ(mapper.locate(0, 0, 3, 0),
              (ChwAddress{1, 8, 0, 0}));
    EXPECT_EQ(mapper.locate(0, 1, 0, 0),
              (ChwAddress{2, 9, 0, 0}));
    EXPECT_EQ(mapper.locate(1, 0, 0, 0),
              (ChwAddress{4, 11, 0, 0}));
    EXPECT_EQ(mapper.locate(1, 1, 3, 4),
              (ChwAddress{7, 14, 4, 4}));
}

TEST(ChwAddressMapper, SplitsW20RowsIntoTwoWords)
{
    auto config = baseConfig();
    config.n = 2;
    config.c = 2;
    config.h = 3;
    config.w = 20;
    config.outH = 3;
    config.outW = 20;
    config.spadBase = 5;
    const ChwAddressMapper mapper(config);

    EXPECT_EQ(mapper.derived().wWords, uint64_t{2});
    EXPECT_EQ(mapper.derived().spatialWordsPerChannel, uint64_t{6});
    EXPECT_EQ(mapper.locate(0, 0, 0, 0),
              (ChwAddress{0, 5, 0, 0}));
    EXPECT_EQ(mapper.locate(0, 0, 0, 16),
              (ChwAddress{1, 6, 0, 0}));
    EXPECT_EQ(mapper.locate(0, 0, 1, 18),
              (ChwAddress{3, 8, 2, 2}));
    EXPECT_EQ(mapper.locate(0, 1, 1, 18),
              (ChwAddress{9, 14, 2, 2}));
    EXPECT_EQ(mapper.locate(1, 1, 2, 19),
              (ChwAddress{23, 28, 3, 3}));
}

TEST(ChwAddressMapper, FreezesW16AndW17BranchBoundary)
{
    auto w16 = baseConfig();
    w16.w = 16;
    w16.outW = 16;
    const ChwAddressMapper packed(w16);
    EXPECT_EQ(packed.locate(0, 0, 0, 15),
              (ChwAddress{0, 0, 15, 15}));

    auto w17 = baseConfig();
    w17.w = 17;
    w17.outW = 17;
    const ChwAddressMapper split(w17);
    EXPECT_EQ(split.locate(0, 0, 0, 15),
              (ChwAddress{0, 0, 15, 15}));
    EXPECT_EQ(split.locate(0, 0, 0, 16),
              (ChwAddress{1, 1, 0, 0}));
}

TEST(ChwAddressMapper, ExposesSameBankSameAndDifferentRowCases)
{
    auto config = baseConfig();
    config.w = 20;
    config.outW = 20;
    const ChwAddressMapper mapper(config);

    const auto first = mapper.locate(0, 0, 0, 0);
    const auto duplicate = mapper.locate(0, 0, 0, 0);
    const auto nextWord = mapper.locate(0, 0, 0, 16);

    EXPECT_EQ(first, duplicate);
    EXPECT_EQ(first.bank, nextWord.bank);
    EXPECT_NE(first.row, nextWord.row);
}

TEST(ChwAddressMapper, RejectsCoordinatesOutsideResolvedTensor)
{
    auto config = baseConfig();
    config.n = 2;
    config.c = 3;
    config.h = 4;
    config.w = 5;
    config.outH = 4;
    config.outW = 5;
    const ChwAddressMapper mapper(config);

    EXPECT_THROW(mapper.locate(2, 0, 0, 0), std::out_of_range);
    EXPECT_THROW(mapper.locate(0, 3, 0, 0), std::out_of_range);
    EXPECT_THROW(mapper.locate(0, 0, 4, 0), std::out_of_range);
    EXPECT_THROW(mapper.locate(0, 0, 0, 5), std::out_of_range);
}

TEST(ChwAddressMapper, RejectsInvalidConfigAtConstruction)
{
    auto config = baseConfig();
    config.spadBase = 4095;
    config.h = 2;
    config.w = 16;
    config.outW = 16;
    EXPECT_THROW(ChwAddressMapper mapper(config), std::invalid_argument);
}

TEST(ChwAddressMapper, ActivationGeneratorKeepsLowEightBits)
{
    EXPECT_EQ(tbActValueV1(0, 0, 0, 0), uint8_t{1});
    const uint64_t expected =
        (uint64_t{2} * 97 + uint64_t{3} * 31 +
         uint64_t{4} * 7 + 5 + 1) & 0xff;
    EXPECT_EQ(tbActValueV1(2, 3, 4, 5),
              static_cast<uint8_t>(expected));
    EXPECT_EQ(tbActValueV1(256, 256, 256, 256), uint8_t{1});
}

} // anonymous namespace
} // namespace gem5::sau_n
