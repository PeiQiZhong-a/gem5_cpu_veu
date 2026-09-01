#ifndef __SAU_MIKUI_SAU_CONSTANTS_HH__
#define __SAU_MIKUI_SAU_CONSTANTS_HH__

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace gem5::sau_mikui
{

// Frozen against mikui-debug-dma e7c05d066fd5bc52f7ec1f31d53d8c5cef5651a8.
struct SauConstants
{
    static constexpr unsigned Rows = 16;
    static constexpr unsigned Cols = 16;
    static constexpr unsigned PeCount = Rows * Cols;
    static constexpr unsigned InputBits = 8;
    static constexpr unsigned AccumulatorBits = 24;
    static constexpr unsigned QuantizedBits = 16;
    static constexpr unsigned SramBeatBits = 128;
    static constexpr unsigned SramBeatBytes = SramBeatBits / 8;
    static constexpr unsigned RegisterDepth = 48;
    static constexpr unsigned TransposerDepth = 16;
    static constexpr unsigned AddressDelay = 6;
    static constexpr unsigned SramDelay = 1;
    static constexpr unsigned MemoryControllerDelay = 2;
    static constexpr unsigned FeederStateDelay =
        SramDelay + AddressDelay + MemoryControllerDelay;
    static constexpr uint32_t BaseAddress = 0x20000000;
};

static_assert(SauConstants::FeederStateDelay == 9);

constexpr uint64_t
maskFor(unsigned bits)
{
    return bits >= 64 ? std::numeric_limits<uint64_t>::max()
                      : ((uint64_t{1} << bits) - 1);
}

constexpr int64_t
signExtend(uint64_t value, unsigned bits)
{
    const uint64_t masked = value & maskFor(bits);
    const uint64_t sign = uint64_t{1} << (bits - 1);
    return static_cast<int64_t>((masked ^ sign) - sign);
}

constexpr int32_t
wrapSigned(int64_t value, unsigned bits)
{
    return static_cast<int32_t>(
        signExtend(static_cast<uint64_t>(value), bits));
}

constexpr int32_t
saturatingAdd24(int32_t lhs, int32_t rhs)
{
    constexpr int32_t Min = -(int32_t{1} << 23);
    constexpr int32_t Max = (int32_t{1} << 23) - 1;
    const int64_t sum =
        static_cast<int64_t>(wrapSigned(lhs, 24)) + wrapSigned(rhs, 24);
    return sum > Max ? Max : (sum < Min ? Min : static_cast<int32_t>(sum));
}

constexpr int32_t
arithmeticShiftRight24(int32_t value, unsigned shift)
{
    const int32_t signedValue = wrapSigned(value, 24);
    if (shift == 0) {
        return signedValue;
    }
    if (signedValue >= 0) {
        return signedValue / (int32_t{1} << shift);
    }
    const int64_t magnitude = -static_cast<int64_t>(signedValue);
    const int64_t divisor = int64_t{1} << shift;
    return static_cast<int32_t>(-((magnitude + divisor - 1) / divisor));
}

constexpr int16_t
saturatingTruncate(int32_t accumulator, unsigned cutbit, bool shiftMode)
{
    const int32_t shifted = arithmeticShiftRight24(accumulator, cutbit & 0x1f);
    const int32_t minimum = shiftMode ? -32768 : -128;
    const int32_t maximum = shiftMode ? 32767 : 127;
    const int32_t saturated =
        shifted < minimum ? minimum : (shifted > maximum ? maximum : shifted);
    return static_cast<int16_t>(saturated);
}

inline void
validateArchitecture(unsigned rows, unsigned cols, unsigned sramDelay)
{
    if (rows != SauConstants::Rows || cols != SauConstants::Cols ||
        sramDelay != SauConstants::SramDelay) {
        throw std::invalid_argument(
            "Mikui SAU only supports the frozen 16x16, SRAM_DELAY=1 RTL");
    }
}

} // namespace gem5::sau_mikui

#endif // __SAU_MIKUI_SAU_CONSTANTS_HH__
