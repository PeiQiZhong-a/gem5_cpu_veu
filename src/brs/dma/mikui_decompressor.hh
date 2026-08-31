#ifndef __BRS_DMA_MIKUI_DECOMPRESSOR_HH__
#define __BRS_DMA_MIKUI_DECOMPRESSOR_HH__

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace gem5
{
namespace brs
{

// Functional representation of the stream consumed by
// hardware/src/DMA/decompress/{meta_bmp_analyzer,rice_decode_stream,
// zigzag_unmap_uints,zeros_skip}.v.  The AHB wrapper supplies 32-bit words;
// metadata is byte-swapped by meta_bmp_analyzer while payload bits are
// consumed MSB first from the unmodified bus word.
class MikuiDecompressor
{
  public:
    static constexpr uint32_t ElementsPerBlock = 512;
    static constexpr uint32_t BitmapWordsPerBlock = 16;
    static constexpr uint32_t MaximumBlocks = 1024;
    static constexpr uint8_t MaximumRiceQuotient = 15;

    struct Metadata
    {
        bool zeroSkip = false;
        uint8_t riceK = 0;
        uint16_t blockCount = 0;
        uint16_t lastBlockElements = 0;
    };

    struct Result
    {
        bool success = false;
        Metadata metadata;
        std::vector<uint32_t> outputWords;
        uint64_t decodedElements = 0;
        std::string error;
    };

    static Result decode(
        const std::vector<uint8_t> &input,
        uint64_t maximumOutputBytes = std::numeric_limits<uint64_t>::max());

  private:
    static uint32_t loadLeWord(
        const std::vector<uint8_t> &input, size_t wordIndex);
};

} // namespace brs
} // namespace gem5

#endif // __BRS_DMA_MIKUI_DECOMPRESSOR_HH__
