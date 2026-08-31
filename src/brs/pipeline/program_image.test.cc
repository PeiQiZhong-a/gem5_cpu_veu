#include "brs/pipeline/program_image.hh"

#include <cstdio>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace gem5
{
namespace
{

TEST(DataImageTest, LoadsReadmemhWordsAsLittleEndianBytes)
{
    const std::string path = ::testing::TempDir() +
        "brs_data_image_readmemh.hex";
    {
        std::ofstream output(path);
        ASSERT_TRUE(output.is_open());
        output << "11223344\n@00000003\naabb_ccdd // word 3\n";
    }

    DataImage image;
    ASSERT_TRUE(image.loadReadmemh32File(path));
    ASSERT_EQ(image.data.size(), 16u);
    EXPECT_EQ(image.data[0], 0x44);
    EXPECT_EQ(image.data[1], 0x33);
    EXPECT_EQ(image.data[2], 0x22);
    EXPECT_EQ(image.data[3], 0x11);
    EXPECT_EQ(image.data[8], 0x00);
    EXPECT_EQ(image.data[12], 0xdd);
    EXPECT_EQ(image.data[13], 0xcc);
    EXPECT_EQ(image.data[14], 0xbb);
    EXPECT_EQ(image.data[15], 0xaa);

    std::remove(path.c_str());
}

TEST(DataImageTest, SupportsTheLastWordOfA65536WordReadmemhImage)
{
    const std::string path = ::testing::TempDir() +
        "brs_data_image_readmemh_capacity.hex";
    {
        std::ofstream output(path);
        ASSERT_TRUE(output.is_open());
        output << "@0000ffff\n89abcdef\n";
    }

    DataImage image;
    ASSERT_TRUE(image.loadReadmemh32File(path));
    ASSERT_EQ(image.data.size(), 65536u * sizeof(uint32_t));
    const size_t last = image.data.size() - sizeof(uint32_t);
    EXPECT_EQ(image.data[last + 0], 0xef);
    EXPECT_EQ(image.data[last + 1], 0xcd);
    EXPECT_EQ(image.data[last + 2], 0xab);
    EXPECT_EQ(image.data[last + 3], 0x89);

    EXPECT_FALSE(image.trimZeroFilledTail(0x30000));

    std::remove(path.c_str());
}

TEST(DataImageTest, TrimsOnlyZeroFillBeyondTheThreeRealSramBanks)
{
    DataImage image;
    image.data.resize(0x40000, 0);
    image.data[0x2ffff] = 0x5a;

    ASSERT_TRUE(image.trimZeroFilledTail(0x30000));
    ASSERT_EQ(image.data.size(), 0x30000u);
    EXPECT_EQ(image.data.back(), 0x5a);
}

TEST(DataImageTest, KeepsLegacyByteTokenFormatSeparate)
{
    const std::string path = ::testing::TempDir() +
        "brs_data_image_bytes.hex";
    {
        std::ofstream output(path);
        ASSERT_TRUE(output.is_open());
        output << "11 22 ff\n";
    }

    DataImage image;
    ASSERT_TRUE(image.loadHexFile(path));
    ASSERT_EQ(image.data.size(), 3u);
    EXPECT_EQ(image.data[0], 0x11);
    EXPECT_EQ(image.data[1], 0x22);
    EXPECT_EQ(image.data[2], 0xff);

    std::remove(path.c_str());
}

} // namespace
} // namespace gem5
