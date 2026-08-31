#include "brs/dma/mikui_decompressor.hh"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace gem5
{
namespace brs
{
namespace
{

void
appendLeWord(std::vector<uint8_t> &bytes, uint32_t word)
{
    for (unsigned byte = 0; byte < 4; ++byte) {
        bytes.push_back(static_cast<uint8_t>(word >> (byte * 8)));
    }
}

void
appendMeta(std::vector<uint8_t> &bytes, bool zeroSkip, uint8_t k,
           uint16_t blocks, uint16_t lastElements)
{
    const uint32_t meta =
        (static_cast<uint32_t>(lastElements) << 20) |
        (static_cast<uint32_t>(blocks) << 4) |
        (static_cast<uint32_t>(k) << 1) |
        static_cast<uint32_t>(zeroSkip);
    bytes.push_back(static_cast<uint8_t>(meta >> 24));
    bytes.push_back(static_cast<uint8_t>(meta >> 16));
    bytes.push_back(static_cast<uint8_t>(meta >> 8));
    bytes.push_back(static_cast<uint8_t>(meta));
}

uint8_t
zigzag(int8_t value)
{
    const int16_t wide = value;
    return value >= 0 ? static_cast<uint8_t>(wide * 2) :
        static_cast<uint8_t>(-wide * 2 - 1);
}

void
appendRiceWords(std::vector<uint8_t> &bytes,
                const std::vector<int8_t> &values, uint8_t k)
{
    std::vector<bool> bits;
    for (const int8_t signedValue : values) {
        const uint8_t value = zigzag(signedValue);
        const uint8_t quotient = value >> k;
        const uint8_t remainder = value & ((uint16_t{1} << k) - 1u);
        for (uint8_t one = 0; one < quotient; ++one) {
            bits.push_back(true);
        }
        bits.push_back(false);
        for (int bit = k - 1; bit >= 0; --bit) {
            bits.push_back((remainder >> bit) & 1u);
        }
    }
    while (bits.size() % 32 != 0) {
        bits.push_back(false);
    }
    for (size_t first = 0; first < bits.size(); first += 32) {
        uint32_t word = 0;
        for (size_t bit = 0; bit < 32; ++bit) {
            word |= static_cast<uint32_t>(bits[first + bit]) << (31 - bit);
        }
        appendLeWord(bytes, word);
    }
}

TEST(MikuiDecompressorTest, DecodesDenseRiceAndRtlWordPacking)
{
    std::vector<uint8_t> input;
    appendMeta(input, false, 2, 1, 4);
    appendRiceWords(input, {0, -1, 1, -2}, 2);

    const auto result = MikuiDecompressor::decode(input);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_FALSE(result.metadata.zeroSkip);
    EXPECT_EQ(result.metadata.riceK, 2);
    EXPECT_EQ(result.metadata.blockCount, 1);
    EXPECT_EQ(result.metadata.lastBlockElements, 4);
    ASSERT_EQ(result.outputWords.size(), 1);
    EXPECT_EQ(result.outputWords[0], 0x00ff01feu);
}

TEST(MikuiDecompressorTest, RestoresZerosFromMsbFirstBitmap)
{
    std::vector<uint8_t> input;
    appendMeta(input, true, 1, 1, 8);
    appendLeWord(input, 0x80000000u); // Block 0 uses zero skipping.
    appendLeWord(input, 0x91000000u); // Elements 0, 3, and 7 are present.
    for (unsigned word = 1; word < 16; ++word) {
        appendLeWord(input, 0);
    }
    appendRiceWords(input, {5, -2, 1}, 1);

    const auto result = MikuiDecompressor::decode(input);
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_EQ(result.outputWords.size(), 2);
    EXPECT_EQ(result.outputWords[0], 0x050000feu);
    EXPECT_EQ(result.outputWords[1], 0x00000001u);
}

TEST(MikuiDecompressorTest, RejectsInvalidMetadataAndTruncatedPayload)
{
    std::vector<uint8_t> invalid;
    appendMeta(invalid, false, 0, 0, 1);
    EXPECT_FALSE(MikuiDecompressor::decode(invalid).success);

    std::vector<uint8_t> truncated;
    appendMeta(truncated, false, 2, 1, 4);
    const auto result = MikuiDecompressor::decode(truncated);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());

    std::vector<uint8_t> oversized;
    appendMeta(oversized, false, 0, 9, 512);
    EXPECT_FALSE(MikuiDecompressor::decode(oversized, 4096).success);

    std::vector<uint8_t> tooManyBlocks;
    appendMeta(tooManyBlocks, false, 0, 1025, 1);
    EXPECT_FALSE(MikuiDecompressor::decode(tooManyBlocks).success);
}

} // anonymous namespace
} // namespace brs
} // namespace gem5
