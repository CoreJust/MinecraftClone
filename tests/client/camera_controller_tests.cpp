#include <client/CameraController.hpp>

#include <shared/world/World.hpp>

#include <gtest/gtest.h>

#include <cmath>

TEST(CameraControllerTest, YawZeroMapsWASDToWorldAxes) {
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .forward = 1 }, 0.0),
        (client::MovementDirection{ .x = 0, .y = 127 })
    );
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .forward = -1 }, 0.0),
        (client::MovementDirection{ .x = 0, .y = -127 })
    );
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .strafe = -1 }, 0.0),
        (client::MovementDirection{ .x = -127, .y = 0 })
    );
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .strafe = 1 }, 0.0),
        (client::MovementDirection{ .x = 127, .y = 0 })
    );
}

TEST(CameraControllerTest, CardinalYawRotatesMovementAroundTheHorizontalPlane) {
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .forward = 1 }, 90.0),
        (client::MovementDirection{ .x = 127, .y = 0 })
    );
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .forward = 1 }, 180.0),
        (client::MovementDirection{ .x = 0, .y = -127 })
    );
}

TEST(CameraControllerTest, FullTurnsAndNegativeYawKeepCardinalDirectionsStable) {
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .forward = 1 }, 360.0),
        (client::MovementDirection{ .x = 0, .y = 127 })
    );
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .forward = 1 }, -90.0),
        (client::MovementDirection{ .x = -127, .y = 0 })
    );
}

TEST(CameraControllerTest, DiagonalInputAndYawProduceNormalizedContinuousDirection) {
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .forward = 1 }, 45.0),
        (client::MovementDirection{ .x = 89, .y = 89 })
    );
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .strafe = 1, .forward = 1 }, 0.0),
        (client::MovementDirection{ .x = 89, .y = 89 })
    );
}

TEST(CameraControllerTest, OpposingAxesCancelBeforeMovementIsMapped) {
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .strafe = 0, .forward = 0 }, 45.0),
        (client::MovementDirection{ })
    );
}

TEST(CameraControllerTest, DiagonalDirectionDoesNotGainSpeed) {
    for (double const yaw : { 0.0, 45.0, 90.0, 180.0, 270.0 }) {
        client::MovementDirection const movement =
            client::CameraController::cameraRelativeMovement({ .strafe = 1, .forward = 1 }, yaw);
        EXPECT_LE(std::hypot(static_cast<double>(movement.x), static_cast<double>(movement.y)), 127.0);
    }
}

TEST(CameraControllerTest, InvalidYawAndIdleInputProduceNoMovement) {
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ }, INFINITY),
        (client::MovementDirection{ })
    );
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ }, 90.0),
        (client::MovementDirection{ })
    );
}

TEST(CameraControllerTest, IntentComponentsAreClampedToDiscreteValues) {
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .forward = 127 }, 0.0),
        (client::MovementDirection{ .x = 0, .y = 127 })
    );
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .strafe = -127 }, 0.0),
        (client::MovementDirection{ .x = -127, .y = 0 })
    );
}

TEST(CameraControllerTest, VerticalFlightInputUsesSpaceOverShiftAndCancelsOpposites)
{
    EXPECT_EQ(client::CameraController::verticalMovement(true, false), 127);
    EXPECT_EQ(client::CameraController::verticalMovement(false, true), -127);
    EXPECT_EQ(client::CameraController::verticalMovement(false, false), 0);
    EXPECT_EQ(client::CameraController::verticalMovement(true, true), 0);
}

TEST(CameraControllerTest, FlightControlsMoveVerticalAndHorizontalAxesAtTheSameSpeed)
{
    static constexpr uint32_t AXIS_STEP = shared::MOVEMENT_SUBCELLS_PER_TICK;
    static constexpr uint32_t DIAGONAL_DIVISOR = 180;
    static constexpr uint32_t DIAGONAL_STEP = shared::MOVEMENT_SUBCELLS_PER_TICK * 127U / DIAGONAL_DIVISOR;

    client::MovementDirection const horizontal =
        client::CameraController::cameraRelativeMovement({ .forward = 1 }, 0.0);
    int8_t const vertical = client::CameraController::verticalMovement(true, false);
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1, '@');
    world.spawnPlayer(2, '#');
    world.spawnPlayer(3, '$');

    ASSERT_TRUE(world.movePlayer(1, {
        .x = static_cast<uint8_t>(horizontal.x),
        .y = static_cast<uint8_t>(horizontal.y),
    }));
    ASSERT_TRUE(world.movePlayer(2, { .z = static_cast<uint8_t>(vertical) }));
    ASSERT_TRUE(world.movePlayer(3, {
        .x = static_cast<uint8_t>(horizontal.x),
        .y = static_cast<uint8_t>(horizontal.y),
        .z = static_cast<uint8_t>(vertical),
    }));
    ASSERT_TRUE(world.player(1).has_value());
    ASSERT_TRUE(world.player(2).has_value());
    ASSERT_TRUE(world.player(3).has_value());

    EXPECT_EQ(world.player(1)->y_subcell, AXIS_STEP);
    EXPECT_EQ(world.player(2)->z_subcell, AXIS_STEP);
    EXPECT_EQ(world.player(3)->y_subcell, DIAGONAL_STEP);
    EXPECT_EQ(world.player(3)->z_subcell, DIAGONAL_STEP);
    EXPECT_LE(
        2U * DIAGONAL_STEP * DIAGONAL_STEP,
        AXIS_STEP * AXIS_STEP
    );
}
