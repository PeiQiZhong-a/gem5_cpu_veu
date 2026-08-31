#include "sau_n/sau_generators.hh"

#include <stdexcept>
#include <string>

namespace gem5::sau_n
{

int8_t
signedInt8(uint8_t raw)
{
    const int16_t value = raw < 128 ? raw : static_cast<int16_t>(raw) - 256;
    return static_cast<int8_t>(value);
}

uint8_t
activationRawV1(uint64_t n, uint64_t c, uint64_t h, uint64_t w)
{
    const uint64_t raw = (n % 256) * 97 + (c % 256) * 31 +
        (h % 256) * 7 + w % 256 + 1;
    return static_cast<uint8_t>(raw % 256);
}

int8_t
activationValueV1(uint64_t n, uint64_t c, uint64_t h, uint64_t w)
{
    return signedInt8(activationRawV1(n, c, h, w));
}

int8_t
weightValue(
    std::string_view generator, uint64_t oc, uint64_t c,
    uint64_t kh, uint64_t kw)
{
    if (generator == "zero") {
        return 0;
    }
    if (generator == "ones") {
        return 1;
    }
    if (generator != "tb_weight_value_v1") {
        throw std::invalid_argument(
            "unsupported weight generator: " + std::string(generator));
    }
    const uint64_t raw = (oc % 255) * 29 + (c % 255) * 17 +
        (kh % 255) * 5 + (kw % 255) * 3 + 11;
    return static_cast<int8_t>(static_cast<int16_t>(raw % 255) - 127);
}

int16_t
biasValue(std::string_view generator, uint64_t oc)
{
    if (generator == "zero") {
        return 0;
    }
    if (generator != "tb_bias_value_v1") {
        throw std::invalid_argument(
            "unsupported bias generator: " + std::string(generator));
    }
    return static_cast<int16_t>((oc % 257) * 37 + 13) % 257 - 128;
}

} // namespace gem5::sau_n
