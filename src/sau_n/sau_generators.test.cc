#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>

#include "sau_n/sau_generators.hh"

namespace gem5::sau_n
{
namespace
{

TEST(SauGenerators, PreservesSignedActivationBitPattern)
{
    EXPECT_EQ(signedInt8(0), int8_t{0});
    EXPECT_EQ(signedInt8(127), int8_t{127});
    EXPECT_EQ(signedInt8(128), int8_t{-128});
    EXPECT_EQ(signedInt8(255), int8_t{-1});
    EXPECT_EQ(activationRawV1(0, 0, 0, 0), uint8_t{1});
    EXPECT_EQ(activationRawV1(1, 1, 1, 1), uint8_t{137});
    EXPECT_EQ(activationValueV1(1, 1, 1, 1), int8_t{-119});
}

TEST(SauGenerators, MatchesFrozenWeightValues)
{
    EXPECT_EQ(weightValue("tb_weight_value_v1", 0, 0, 0, 0), int8_t{-116});
    EXPECT_EQ(weightValue("tb_weight_value_v1", 15, 2, 2, 2), int8_t{114});
    EXPECT_EQ(weightValue("zero", 9, 8, 2, 1), int8_t{0});
    EXPECT_EQ(weightValue("ones", 9, 8, 2, 1), int8_t{1});
    EXPECT_THROW(weightValue("random", 0, 0, 0, 0), std::invalid_argument);
}

TEST(SauGenerators, MatchesFrozenBiasValues)
{
    EXPECT_EQ(biasValue("tb_bias_value_v1", 0), int16_t{-115});
    EXPECT_EQ(biasValue("tb_bias_value_v1", 15), int16_t{-74});
    EXPECT_EQ(biasValue("zero", 15), int16_t{0});
    EXPECT_THROW(biasValue("ones", 0), std::invalid_argument);
}

} // anonymous namespace
} // namespace gem5::sau_n
