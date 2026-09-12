#include <client/CameraController.hpp>

#include <AndroidInputState.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace game_android {
namespace {

[[nodiscard]]
int8_t signedComponent(uint8_t const component) noexcept
{
    return static_cast<int8_t>(component);
}

TEST(AndroidInputStateTest, TracksKeyPressesReleasesAndCombinedAxes)
{
    AndroidInputState input;

    input.setMoveKeyPressed(AndroidMoveKey::Left, true);
    input.setMoveKeyPressed(AndroidMoveKey::Up, true);
    EXPECT_EQ(signedComponent(input.direction().x), -1);
    EXPECT_EQ(signedComponent(input.direction().y), -1);

    input.setMoveKeyPressed(AndroidMoveKey::Left, false);
    input.setMoveKeyPressed(AndroidMoveKey::Right, true);
    input.setMoveKeyPressed(AndroidMoveKey::Down, true);
    EXPECT_EQ(signedComponent(input.direction().x), 1);
    EXPECT_EQ(signedComponent(input.direction().y), 0);

    input.setMoveKeyPressed(AndroidMoveKey::Right, false);
    input.setMoveKeyPressed(AndroidMoveKey::Up, false);
    input.setMoveKeyPressed(AndroidMoveKey::Down, false);
    EXPECT_EQ(signedComponent(input.direction().x), 0);
    EXPECT_EQ(signedComponent(input.direction().y), 0);
}

TEST(AndroidInputStateTest, OpposingHardwareAxesCancelBeforeCameraRelativeMovement)
{
    static constexpr int32_t POINTER_ID = 7;
    static constexpr uint32_t SURFACE_WIDTH = 1'000;
    AndroidInputState input;
    ASSERT_TRUE(input.beginTouch(POINTER_ID, 200.0F, 300.0F, SURFACE_WIDTH));
    ASSERT_TRUE(input.moveTouch(POINTER_ID, 250.0F, 350.0F));
    input.setMoveKeyPressed(AndroidMoveKey::Left, true);
    input.setMoveKeyPressed(AndroidMoveKey::Right, true);
    input.setMoveKeyPressed(AndroidMoveKey::Up, true);
    input.setMoveKeyPressed(AndroidMoveKey::Down, true);

    shared::Direction const direction = input.direction();
    EXPECT_EQ(signedComponent(direction.x), 0);
    EXPECT_EQ(signedComponent(direction.y), 0);
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement(
            {
                .strafe = static_cast<int8_t>(direction.x),
                .forward = static_cast<int8_t>(-static_cast<int8_t>(direction.y)),
            },
            90.0
        ),
        (client::MovementDirection{ })
    );
}

TEST(AndroidInputStateTest, ScalesTouchDeadZoneWithDensity)
{
    static constexpr int32_t POINTER_ID = 7;
    static constexpr uint32_t SURFACE_WIDTH = 1'000;
    static constexpr float ORIGIN_X = 200.0f;
    static constexpr float ORIGIN_Y = 300.0f;

    AndroidInputState mdpi;
    ASSERT_TRUE(mdpi.beginTouch(POINTER_ID, ORIGIN_X, ORIGIN_Y, SURFACE_WIDTH));
    EXPECT_TRUE(mdpi.moveTouch(POINTER_ID, ORIGIN_X + 24.0f, ORIGIN_Y - 24.0f));
    EXPECT_EQ(signedComponent(mdpi.direction().x), 0);
    EXPECT_EQ(signedComponent(mdpi.direction().y), 0);
    EXPECT_TRUE(mdpi.moveTouch(POINTER_ID, ORIGIN_X + 25.0f, ORIGIN_Y - 25.0f));
    EXPECT_EQ(signedComponent(mdpi.direction().x), 1);
    EXPECT_EQ(signedComponent(mdpi.direction().y), -1);

    AndroidInputState xhdpi;
    xhdpi.setDensity(2.0f);
    ASSERT_TRUE(xhdpi.beginTouch(POINTER_ID, ORIGIN_X, ORIGIN_Y, SURFACE_WIDTH));
    EXPECT_TRUE(xhdpi.moveTouch(POINTER_ID, ORIGIN_X + 48.0f, ORIGIN_Y - 48.0f));
    EXPECT_EQ(signedComponent(xhdpi.direction().x), 0);
    EXPECT_EQ(signedComponent(xhdpi.direction().y), 0);
    EXPECT_TRUE(xhdpi.moveTouch(POINTER_ID, ORIGIN_X + 50.0f, ORIGIN_Y - 50.0f));
    EXPECT_EQ(signedComponent(xhdpi.direction().x), 1);
    EXPECT_EQ(signedComponent(xhdpi.direction().y), -1);
}

TEST(AndroidInputStateTest, RejectsInvalidDensityScale)
{
    static constexpr int32_t POINTER_ID = 7;
    static constexpr uint32_t SURFACE_WIDTH = 1'000;

    AndroidInputState input;
    input.setDensity(std::numeric_limits<float>::infinity());
    ASSERT_TRUE(input.beginTouch(POINTER_ID, 200.0f, 300.0f, SURFACE_WIDTH));
    ASSERT_TRUE(input.moveTouch(POINTER_ID, 225.0f, 300.0f));
    EXPECT_EQ(signedComponent(input.direction().x), 1);
}

TEST(AndroidInputStateTest, ReleasesAndCancelsOnlyTheTrackedTouch)
{
    static constexpr int32_t POINTER_ID = 7;
    static constexpr int32_t OTHER_POINTER_ID = 8;
    static constexpr uint32_t SURFACE_WIDTH = 1'000;

    AndroidInputState input;
    EXPECT_FALSE(input.beginTouch(POINTER_ID, 750.0f, 300.0f, SURFACE_WIDTH));
    ASSERT_TRUE(input.beginTouch(POINTER_ID, 200.0f, 300.0f, SURFACE_WIDTH));
    EXPECT_FALSE(input.beginTouch(OTHER_POINTER_ID, 100.0f, 100.0f, SURFACE_WIDTH));
    ASSERT_TRUE(input.moveTouch(POINTER_ID, 250.0f, 350.0f));
    EXPECT_FALSE(input.endTouch(OTHER_POINTER_ID));
    EXPECT_EQ(signedComponent(input.direction().x), 1);
    EXPECT_EQ(signedComponent(input.direction().y), 1);

    EXPECT_TRUE(input.endTouch(POINTER_ID));
    EXPECT_EQ(signedComponent(input.direction().x), 0);
    EXPECT_EQ(signedComponent(input.direction().y), 0);

    ASSERT_TRUE(input.beginTouch(POINTER_ID, 200.0f, 300.0f, SURFACE_WIDTH));
    ASSERT_TRUE(input.moveTouch(POINTER_ID, 150.0f, 250.0f));
    EXPECT_TRUE(input.cancelTouch());
    EXPECT_EQ(signedComponent(input.direction().x), 0);
    EXPECT_EQ(signedComponent(input.direction().y), 0);
    EXPECT_FALSE(input.cancelTouch());
}

TEST(AndroidInputStateTest, RejectsNonFiniteTouchCoordinates)
{
    static constexpr int32_t POINTER_ID = 7;
    static constexpr uint32_t SURFACE_WIDTH = 1'000;
    static constexpr float ORIGIN_X = 200.0f;
    static constexpr float ORIGIN_Y = 300.0f;

    AndroidInputState input;
    EXPECT_FALSE(input.beginTouch(
        POINTER_ID,
        std::numeric_limits<float>::quiet_NaN(),
        ORIGIN_Y,
        SURFACE_WIDTH
    ));
    EXPECT_FALSE(input.beginTouch(
        POINTER_ID,
        ORIGIN_X,
        std::numeric_limits<float>::infinity(),
        SURFACE_WIDTH
    ));

    ASSERT_TRUE(input.beginTouch(POINTER_ID, ORIGIN_X, ORIGIN_Y, SURFACE_WIDTH));
    EXPECT_TRUE(input.moveTouch(POINTER_ID, std::numeric_limits<float>::quiet_NaN(), ORIGIN_Y));
    EXPECT_EQ(signedComponent(input.direction().x), 0);
    EXPECT_EQ(signedComponent(input.direction().y), 0);
    EXPECT_FALSE(input.moveTouch(POINTER_ID, ORIGIN_X + 50.0f, ORIGIN_Y));
}

TEST(AndroidInputStateTest, HardwareKeysOverrideTouchPerAxisUntilReleased)
{
    static constexpr int32_t POINTER_ID = 7;
    static constexpr uint32_t SURFACE_WIDTH = 1'000;

    AndroidInputState input;
    ASSERT_TRUE(input.beginTouch(POINTER_ID, 200.0f, 300.0f, SURFACE_WIDTH));
    ASSERT_TRUE(input.moveTouch(POINTER_ID, 150.0f, 350.0f));
    input.setMoveKeyPressed(AndroidMoveKey::Right, true);
    EXPECT_EQ(signedComponent(input.direction().x), 1);
    EXPECT_EQ(signedComponent(input.direction().y), 1);

    input.setMoveKeyPressed(AndroidMoveKey::Right, false);
    EXPECT_EQ(signedComponent(input.direction().x), -1);
    EXPECT_EQ(signedComponent(input.direction().y), 1);

    input.clear();
    EXPECT_EQ(signedComponent(input.direction().x), 0);
    EXPECT_EQ(signedComponent(input.direction().y), 0);
}

TEST(AndroidInputStateTest, TracksMovementAndLookForTwoPointersInEitherMoveOrder)
{
    static constexpr uint32_t SURFACE_WIDTH = 1'000;
    AndroidInputState input;
    ASSERT_TRUE(input.beginTouch(7, 200.0F, 300.0F, SURFACE_WIDTH));
    ASSERT_TRUE(input.beginLookTouch(8, 700.0F, 300.0F, SURFACE_WIDTH));

    EXPECT_TRUE(input.moveLookTouch(8, 725.0F, 280.0F));
    EXPECT_TRUE(input.moveTouch(7, 250.0F, 350.0F));
    EXPECT_EQ(signedComponent(input.direction().x), 1);
    EXPECT_EQ(signedComponent(input.direction().y), 1);
    float horizontal = 0.0F;
    float vertical = 0.0F;
    EXPECT_TRUE(input.consumeLookDelta(horizontal, vertical));
    EXPECT_FLOAT_EQ(horizontal, 25.0F);
    EXPECT_FLOAT_EQ(vertical, -20.0F);

    EXPECT_TRUE(input.moveTouch(7, 150.0F, 250.0F));
    EXPECT_TRUE(input.moveLookTouch(8, 735.0F, 300.0F));
    EXPECT_EQ(signedComponent(input.direction().x), -1);
    EXPECT_EQ(signedComponent(input.direction().y), -1);
    EXPECT_TRUE(input.consumeLookDelta(horizontal, vertical));
    EXPECT_FLOAT_EQ(horizontal, 10.0F);
    EXPECT_FLOAT_EQ(vertical, 20.0F);
}

} // namespace
} // namespace game_android
