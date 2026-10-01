#include <AndroidInput.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace game_android {
namespace {

TEST(AndroidInputTest, ReservesRightEdgeTopAndBottomBandsForFlight)
{
    static constexpr uint32_t SURFACE_WIDTH = 1'000U;
    static constexpr uint32_t SURFACE_HEIGHT = 2'000U;

    EXPECT_EQ(
        AndroidFlightTouchControls::direction(900.0F, 100.0F, SURFACE_WIDTH, SURFACE_HEIGHT),
        1
    );
    EXPECT_EQ(
        AndroidFlightTouchControls::direction(900.0F, 1'900.0F, SURFACE_WIDTH, SURFACE_HEIGHT),
        -1
    );
    EXPECT_FALSE(AndroidFlightTouchControls::direction(700.0F, 100.0F, SURFACE_WIDTH, SURFACE_HEIGHT));
    EXPECT_FALSE(AndroidFlightTouchControls::direction(900.0F, 1'000.0F, SURFACE_WIDTH, SURFACE_HEIGHT));
}

TEST(AndroidInputTest, RejectsInvalidFlightTouchCoordinates)
{
    static constexpr uint32_t SURFACE_WIDTH = 1'000U;
    static constexpr uint32_t SURFACE_HEIGHT = 2'000U;

    EXPECT_FALSE(AndroidFlightTouchControls::direction(
        std::numeric_limits<float>::quiet_NaN(),
        100.0F,
        SURFACE_WIDTH,
        SURFACE_HEIGHT
    ));
    EXPECT_FALSE(AndroidFlightTouchControls::direction(
        900.0F,
        std::numeric_limits<float>::infinity(),
        SURFACE_WIDTH,
        SURFACE_HEIGHT
    ));
    EXPECT_FALSE(AndroidFlightTouchControls::direction(900.0F, 100.0F, 0U, SURFACE_HEIGHT));
    EXPECT_FALSE(AndroidFlightTouchControls::direction(900.0F, 100.0F, SURFACE_WIDTH, 0U));
}

TEST(AndroidInputTest, FlightAscendTapIsLatchedUntilTheNextSimulationInput)
{
    AndroidFlightInputLatch input;
    input.setPressed(true);
    input.setPressed(false);

    EXPECT_TRUE(input.consumePress());
    EXPECT_FALSE(input.consumePress());

    input.setPressed(true);
    EXPECT_TRUE(input.isPressed());
    EXPECT_TRUE(input.consumePress());
}

TEST(AndroidInputTest, FlightAscendLatchClearDropsPendingTapAfterConnectionReset)
{
    AndroidFlightInputLatch input;
    input.setPressed(true);
    input.clear();

    EXPECT_FALSE(input.isPressed());
    EXPECT_FALSE(input.consumePress());
}

TEST(AndroidInputTest, FlightAscendCancelDropsPendingTapBeforeNextSimulationInput)
{
    AndroidFlightInputLatch input;
    input.setPressed(true);
    input.cancel();

    EXPECT_FALSE(input.isPressed());
    EXPECT_FALSE(input.consumePress());
}

} // namespace
} // namespace game_android
