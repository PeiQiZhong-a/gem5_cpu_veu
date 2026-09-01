#include "sau_mikui/sau_transposer.hh"

#include <gtest/gtest.h>

namespace gem5::sau_mikui
{
namespace
{

void
tick(SauTransposer &transposer, const SauTransposerInputs &inputs = {})
{
    transposer.computeNext(inputs);
    transposer.commit();
}

TEST(SauTransposerTest, DoesNotReadUntilSixteenRowsAreLoaded)
{
    SauTransposer transposer;
    transposer.reset();
    SauTransposerInputs inputs;
    inputs.readEnable = true;
    tick(transposer, inputs);
    EXPECT_FALSE(transposer.evaluate().outputReady);

    for (unsigned row = 0; row < SauConstants::Rows; ++row) {
        inputs = {};
        inputs.writeEnable = true;
        for (unsigned col = 0; col < SauConstants::Cols; ++col) {
            inputs.row[col] = static_cast<int8_t>(row * 16 + col);
        }
        tick(transposer, inputs);
    }
    EXPECT_TRUE(transposer.evaluate().outputReady);
    EXPECT_FALSE(transposer.evaluate().inputReady);
}

TEST(SauTransposerTest, ProducesRtlNormalAndTransposeOrders)
{
    auto load = [](SauTransposer &transposer) {
        for (unsigned row = 0; row < SauConstants::Rows; ++row) {
            SauTransposerInputs inputs;
            inputs.writeEnable = true;
            for (unsigned col = 0; col < SauConstants::Cols; ++col) {
                inputs.row[col] = static_cast<int8_t>(row * 16 + col);
            }
            tick(transposer, inputs);
        }
    };

    SauTransposer normal;
    normal.reset();
    load(normal);
    SauTransposerInputs input;
    input.readEnable = true;
    tick(normal, input);
    ASSERT_TRUE(normal.evaluate().valid);
    for (unsigned col = 0; col < SauConstants::Cols; ++col) {
        EXPECT_EQ(static_cast<uint8_t>(normal.evaluate().row[col]), col);
    }

    SauTransposer transposed;
    transposed.reset();
    load(transposed);
    input.transpose = true;
    tick(transposed, input);
    ASSERT_TRUE(transposed.evaluate().valid);
    for (unsigned element = 0; element < SauConstants::Rows; ++element) {
        EXPECT_EQ(static_cast<uint8_t>(transposed.evaluate().row[element]),
                  static_cast<uint8_t>((15 - element) * 16));
    }
}

TEST(SauTransposerTest, LastAndBusyWriteFollowStickyRtlBehavior)
{
    SauTransposer transposer;
    transposer.reset();
    for (unsigned row = 0; row < SauConstants::Rows; ++row) {
        SauTransposerInputs input;
        input.writeEnable = true;
        tick(transposer, input);
    }
    SauTransposerInputs input;
    input.writeEnable = true;
    tick(transposer, input);
    EXPECT_TRUE(transposer.evaluate().error);

    input = {};
    input.readEnable = true;
    for (unsigned row = 0; row < SauConstants::Rows; ++row) {
        tick(transposer, input);
    }
    EXPECT_TRUE(transposer.evaluate().valid);
    EXPECT_TRUE(transposer.evaluate().last);
    tick(transposer);
    EXPECT_TRUE(transposer.evaluate().inputReady);
    EXPECT_FALSE(transposer.evaluate().outputReady);
}

} // anonymous namespace
} // namespace gem5::sau_mikui
