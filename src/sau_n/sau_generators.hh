#ifndef __SAU_N_SAU_GENERATORS_HH__
#define __SAU_N_SAU_GENERATORS_HH__

#include <cstdint>
#include <string_view>

namespace gem5::sau_n
{

int8_t signedInt8(uint8_t raw);
uint8_t activationRawV1(uint64_t n, uint64_t c, uint64_t h, uint64_t w);
int8_t activationValueV1(uint64_t n, uint64_t c, uint64_t h, uint64_t w);
int8_t weightValue(
    std::string_view generator, uint64_t oc, uint64_t c,
    uint64_t kh, uint64_t kw);
int16_t biasValue(std::string_view generator, uint64_t oc);

} // namespace gem5::sau_n

#endif // __SAU_N_SAU_GENERATORS_HH__
