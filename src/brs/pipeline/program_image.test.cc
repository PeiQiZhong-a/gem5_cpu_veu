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
