#include <client/CameraController.hpp>

#include <cmath>

namespace client {

namespace {

constexpr double DEGREES_TO_RADIANS = 0.017'453'292'519'943'295'769'236'907'684'89;
constexpr double FULL_ROTATION_DEGREES = 360.0;
constexpr double AXIS_TIE_EPSILON = 1e-12;

int8_t clampAxis(int8_t const value) noexcept {
    if (value < 0) {
        return -1;
    }
    if (value > 0) {
        return 1;
    }
    return 0;
}

double normalizeYaw(double yaw_degrees) noexcept {
    yaw_degrees = std::fmod(yaw_degrees, FULL_ROTATION_DEGREES);
    if (yaw_degrees < 0.0) {
        yaw_degrees += FULL_ROTATION_DEGREES;
    }
    return yaw_degrees == 0.0 ? 0.0 : yaw_degrees;
}

} // namespace

DiscreteMovement CameraController::cameraRelativeMovement(
    MovementIntent const intent,
    double const yaw_degrees
) noexcept {
    if (!std::isfinite(yaw_degrees)) {
        return { };
    }

    double const yaw_radians = normalizeYaw(yaw_degrees) * DEGREES_TO_RADIANS;
    double const forward_x = std::sin(yaw_radians);
    double const forward_y = std::cos(yaw_radians);
    double const right_x = std::cos(yaw_radians);
    double const right_y = -std::sin(yaw_radians);
    double const world_x = static_cast<double>(clampAxis(intent.strafe)) * right_x
        + static_cast<double>(clampAxis(intent.forward)) * forward_x;
    double const world_y = static_cast<double>(clampAxis(intent.strafe)) * right_y
        + static_cast<double>(clampAxis(intent.forward)) * forward_y;

    double const absolute_x = std::abs(world_x);
    double const absolute_y = std::abs(world_y);
    if (absolute_x == 0.0 && absolute_y == 0.0) {
        return { };
    }
    if (absolute_x == 0.0) {
        return DiscreteMovement{ .x = 0, .y = sign(world_y) };
    }
    if (absolute_y == 0.0) {
        return DiscreteMovement{ .x = sign(world_x), .y = 0 };
    }
    if (absolute_x + AXIS_TIE_EPSILON >= absolute_y) {
        return DiscreteMovement{ .x = sign(world_x), .y = 0 };
    }
    return DiscreteMovement{ .x = 0, .y = sign(world_y) };
}

int8_t CameraController::sign(double const value) noexcept {
    return value < 0.0 ? -1 : 1;
}

} // namespace client
