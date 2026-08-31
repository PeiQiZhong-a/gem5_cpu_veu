#include "brs/dma/mikui_decompressor.hh"

#include <algorithm>
#include <array>
#include <limits>

namespace gem5
{
namespace brs
{

namespace
{

uint32_t
byteSwap32(uint32_t value)
{
    return ((value & 0x000000ffu) << 24) |
           ((value & 0x0000ff00u) << 8) |
           ((value & 0x00ff0000u) >> 8) |
           ((value & 0xff000000u) >> 24);
}

class WordBitReader
{
  public:
    WordBitReader(const std::vector<uint8_t> &input, size_t firstWord)
      : input(input), word(firstWord)
    {}

    bool
    readBit(bool &value)
    {
        if (word >= input.size() / sizeof(uint32_t)) {
            return false;
        }
        const uint32_t data = MikuiDecompressorAccess::load(input, word);
        value = (data >> (31 - bit)) & 1u;
        if (++bit == 32) {
            bit = 0;
            ++word;
        }
        return true;
    }

    bool
    readBits(uint8_t count, uint32_t &value)
    {
        value = 0;
        for (uint8_t index = 0; index < count; ++index) {
            bool bitValue = false;
            if (!readBit(bitValue)) {
                return false;
            }
            value = (value << 1) | static_cast<uint32_t>(bitValue);
        }
        return true;
    }

    size_t
    nextWord() const
    {
        return word + (bit != 0 ? 1 : 0);
    }

  private:
    // Keep the RTL word interpretation local without exposing the decoder's
    // implementation helper as public API.
    struct MikuiDecompressorAccess
    {
        static uint32_t
        load(const std::vector<uint8_t> &data, size_t index)
        {
            const size_t byte = index * sizeof(uint32_t);
            return static_cast<uint32_t>(data[byte]) |
                (static_cast<uint32_t>(data[byte + 1]) << 8) |
                (static_cast<uint32_t>(data[byte + 2]) << 16) |
                (static_cast<uint32_t>(data[byte + 3]) << 24);
        }
    };

    const std::vector<uint8_t> &input;
    size_t word;
    uint8_t bit = 0;
};

bool
decodeRiceValue(WordBitReader &bits, uint8_t k, uint8_t &value,
                std::string &error)
{
    uint8_t quotient = 0;
    bool bit = false;
    while (true) {
        if (!bits.readBit(bit)) {
            error = "compressed stream ended inside a Rice quotient";
            return false;
        }
        if (!bit) {
            break;
        }
        if (++quotient > MikuiDecompressor::MaximumRiceQuotient) {
            error = "Rice quotient exceeds the RTL maximum of 15";
            return false;
        }
    }

    uint32_t remainder = 0;
    if (!bits.readBits(k, remainder)) {
        error = "compressed stream ended inside a Rice remainder";
        return false;
    }

    const uint32_t unsignedValue =
        (static_cast<uint32_t>(quotient) << k) | remainder;
    if (unsignedValue > std::numeric_limits<uint8_t>::max()) {
        error = "Rice value exceeds the RTL 8-bit output width";
        return false;
    }
    // Exact zigzag_unmap_uints.v equation:
    // (unit >> 1) ^ {8{unit[0]}}.
    const uint8_t unit = static_cast<uint8_t>(unsignedValue);
    value = static_cast<uint8_t>((unit >> 1) ^
        (unit & 1u ? uint8_t{0xff} : uint8_t{0x00}));
    return true;
}

} // anonymous namespace

uint32_t
MikuiDecompressor::loadLeWord(
    const std::vector<uint8_t> &input, size_t wordIndex)
{
    const size_t byte = wordIndex * sizeof(uint32_t);
    return static_cast<uint32_t>(input[byte]) |
        (static_cast<uint32_t>(input[byte + 1]) << 8) |
        (static_cast<uint32_t>(input[byte + 2]) << 16) |
        (static_cast<uint32_t>(input[byte + 3]) << 24);
}

MikuiDecompressor::Result
MikuiDecompressor::decode(
    const std::vector<uint8_t> &input, uint64_t maximumOutputBytes)
{
    Result result;
    if (input.size() < sizeof(uint32_t) || input.size() % 4 != 0) {
        result.error = "input length must be a non-zero multiple of four";
        return result;
    }

    // meta_bmp_analyzer.v forms meta_u32 by reversing the AHB word's bytes.
    const uint32_t meta = byteSwap32(loadLeWord(input, 0));
    result.metadata.zeroSkip = meta & 1u;
    result.metadata.riceK = static_cast<uint8_t>((meta >> 1) & 0x7u);
    result.metadata.blockCount = static_cast<uint16_t>((meta >> 4) & 0xffffu);
    result.metadata.lastBlockElements =
        static_cast<uint16_t>((meta >> 20) & 0xfffu);

    if (result.metadata.blockCount == 0) {
        result.error = "metadata block_count is zero";
        return result;
    }
    if (result.metadata.blockCount > MaximumBlocks) {
        result.error = "metadata block_count exceeds the RTL 10-bit block index";
        return result;
    }
    if (result.metadata.lastBlockElements == 0 ||
        result.metadata.lastBlockElements > ElementsPerBlock) {
        result.error = "metadata last_block_elems is outside 1..512";
        return result;
    }

    const uint64_t decodedElements =
        static_cast<uint64_t>(result.metadata.blockCount - 1) *
            ElementsPerBlock +
        result.metadata.lastBlockElements;
    const uint64_t paddedOutputBytes = (decodedElements + 3u) & ~uint64_t{3};
    if (paddedOutputBytes > maximumOutputBytes) {
        result.error = "decoded output exceeds the configured destination capacity";
        return result;
    }

    size_t cursor = 1;
    std::vector<bool> sparseBlocks(result.metadata.blockCount, false);
    if (result.metadata.zeroSkip) {
        const size_t infoWords =
            (result.metadata.blockCount + 31u) / 32u;
        if (cursor + infoWords > input.size() / 4) {
            result.error = "compressed stream ended in block-info bitmap";
            return result;
        }
        for (uint32_t block = 0; block < result.metadata.blockCount; ++block) {
            const uint32_t info = loadLeWord(input, cursor + block / 32u);
            sparseBlocks[block] = (info >> (31 - block % 32u)) & 1u;
        }
        cursor += infoWords;
    }

    for (uint32_t block = 0; block < result.metadata.blockCount; ++block) {
        const uint32_t outputElements =
            block + 1 == result.metadata.blockCount ?
            result.metadata.lastBlockElements : ElementsPerBlock;
        std::array<bool, ElementsPerBlock> present;
        present.fill(true);

        if (sparseBlocks[block]) {
            if (cursor + BitmapWordsPerBlock > input.size() / 4) {
                result.error = "compressed stream ended in a zero-skip bitmap";
                return result;
            }
            for (uint32_t element = 0; element < ElementsPerBlock; ++element) {
                const uint32_t bitmap =
                    loadLeWord(input, cursor + element / 32u);
                present[element] =
                    (bitmap >> (31 - element % 32u)) & 1u;
            }
            cursor += BitmapWordsPerBlock;
        }

        WordBitReader bits(input, cursor);
        std::vector<uint8_t> blockValues(outputElements, 0);
        for (uint32_t element = 0; element < outputElements; ++element) {
            if (!present[element]) {
                continue;
            }
            if (!decodeRiceValue(bits, result.metadata.riceK,
                                 blockValues[element], result.error)) {
                return result;
            }
        }
        cursor = bits.nextWord();

        // zeros_skip.v emits groups of four elements as
        // {element0, element1, element2, element3} on 32-bit HWDATA.
        for (uint32_t element = 0; element < outputElements; element += 4) {
            uint32_t word = 0;
            for (uint32_t lane = 0; lane < 4; ++lane) {
                const uint32_t index = element + lane;
                const uint8_t value =
                    index < outputElements ? blockValues[index] : 0;
                word |= static_cast<uint32_t>(value) << (24 - lane * 8);
            }
            result.outputWords.push_back(word);
        }
        result.decodedElements += outputElements;
    }

    result.success = true;
    return result;
}

} // namespace brs
} // namespace gem5
