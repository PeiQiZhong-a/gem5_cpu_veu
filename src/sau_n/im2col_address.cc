#include "sau_n/im2col_address.hh"

#include <stdexcept>
#include <string>

namespace gem5::sau_n
{
namespace
{

void
requireCoordinate(uint64_t value, uint64_t extent, const char *name)
{
    if (value >= extent) {
        throw std::out_of_range(
            std::string(name) + " coordinate is outside the input tensor");
    }
}

} // anonymous namespace

ChwAddressMapper::ChwAddressMapper(const ResolvedConfig &config)
    : resolved(config), dimensions(validateAndDerive(resolved))
{
}

ChwAddress
ChwAddressMapper::locate(
    uint64_t n, uint64_t c, uint64_t h, uint64_t w) const
{
    requireCoordinate(n, resolved.n, "n");
    requireCoordinate(c, resolved.c, "c");
    requireCoordinate(h, resolved.h, "h");
    requireCoordinate(w, resolved.w, "w");

    const uint64_t nStride = checkedMultiply(
        resolved.c, dimensions.spatialWordsPerChannel, "N word stride");
    uint64_t wordOffset = checkedMultiply(n, nStride, "CHW word offset");
    wordOffset = checkedAdd(
        wordOffset,
        checkedMultiply(
            c, dimensions.spatialWordsPerChannel, "CHW word offset"),
        "CHW word offset");

    uint64_t lane = 0;
    if (resolved.w <= BlockSize) {
        wordOffset = checkedAdd(
            wordOffset, h / dimensions.rowsPerWord, "CHW word offset");
        lane = (h % dimensions.rowsPerWord) * resolved.w + w;
    } else {
        wordOffset = checkedAdd(
            wordOffset,
            checkedMultiply(h, dimensions.wWords, "CHW word offset"),
            "CHW word offset");
        wordOffset = checkedAdd(
            wordOffset, w / BlockSize, "CHW word offset");
        lane = w % BlockSize;
    }

    const uint64_t row = checkedAdd(
        resolved.spadBase, wordOffset, "CHW scratchpad row");
    if (lane >= SpBanks || row >= SpBankEntries) {
        throw std::out_of_range("CHW address exceeds the fixed scratchpad");
    }

    return {
        wordOffset,
        static_cast<uint16_t>(row),
        static_cast<uint8_t>(lane),
        static_cast<uint8_t>(lane),
    };
}

uint8_t
tbActValueV1(uint64_t n, uint64_t c, uint64_t h, uint64_t w)
{
    const uint64_t value =
        (n % 256) * 97 + (c % 256) * 31 +
        (h % 256) * 7 + (w % 256) + 1;
    return static_cast<uint8_t>(value & 0xff);
}

} // namespace gem5::sau_n
