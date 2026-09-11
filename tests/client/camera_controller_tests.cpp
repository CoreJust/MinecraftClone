#include <client/CameraController.hpp>

#include <gtest/gtest.h>

#include <cmath>

TEST(CameraControllerTest, ZeroYawMapsWASDToAuthoritativeCardinalAxes) {
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .forward = 1 }, 0.0),
        (client::DiscreteMovement{ .x = 0, .y = 1 })
    );
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .forward = -1 }, 0.0),
        (client::DiscreteMovement{ .x = 0, .y = -1 })
    );
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .strafe = -1 }, 0.0),
        (client::DiscreteMovement{ .x = -1, .y = 0 })
    );
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .strafe = 1 }, 0.0),
        (client::DiscreteMovement{ .x = 1, .y = 0 })
    );
}

TEST(CameraControllerTest, CardinalYawRotatesMovementIntoWorldAxes) {
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .forward = 1 }, 90.0),
        (client::DiscreteMovement{ .x = 1, .y = 0 })
    );
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .strafe = 1 }, 90.0),
        (client::DiscreteMovement{ .x = 0, .y = -1 })
    );
}

TEST(CameraControllerTest, FullTurnsAndNegativeYawKeepCardinalDirectionsStable) {
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .forward = 1 }, 360.0),
        (client::DiscreteMovement{ .x = 0, .y = 1 })
    );
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .forward = 1 }, -90.0),
        (client::DiscreteMovement{ .x = -1, .y = 0 })
    );
}

TEST(CameraControllerTest, EqualProjectedComponentsUseStableXBreak) {
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .forward = 1 }, 45.0),
        (client::DiscreteMovement{ .x = 1, .y = 0 })
    );
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .strafe = 1, .forward = 1 }, 0.0),
        (client::DiscreteMovement{ .x = 1, .y = 0 })
    );
}

TEST(CameraControllerTest, OpposingRelativeInputsResolveToTheRemainingCardinalAxis) {
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .strafe = 1, .forward = -1 }, 45.0),
        (client::DiscreteMovement{ .x = 0, .y = -1 })
    );
}

TEST(CameraControllerTest, MovementDoesNotInventDiagonalAuthority) {
    for (double const yaw : { 0.0, 45.0, 90.0, 180.0, 270.0 }) {
        client::DiscreteMovement const movement =
            client::CameraController::cameraRelativeMovement({ .strafe = 1, .forward = 1 }, yaw);
        EXPECT_LE(std::abs(movement.x) + std::abs(movement.y), 1);
    }
}

TEST(CameraControllerTest, InvalidYawAndIdleInputProduceNoMovement) {
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ }, INFINITY),
        (client::DiscreteMovement{ })
    );
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ }, 90.0),
        (client::DiscreteMovement{ })
    );
}

TEST(CameraControllerTest, IntentComponentsAreClampedToDiscreteValues) {
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .forward = 127 }, 0.0),
        (client::DiscreteMovement{ .x = 0, .y = 1 })
    );
    EXPECT_EQ(
        client::CameraController::cameraRelativeMovement({ .strafe = -127 }, 0.0),
        (client::DiscreteMovement{ .x = -1, .y = 0 })
    );
}
