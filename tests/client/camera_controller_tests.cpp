#include <client/CameraController.hpp>

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
