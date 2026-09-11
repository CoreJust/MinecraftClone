#pragma once

#include <cstdint>

namespace client {

struct MovementIntent final {
    int8_t strafe = 0;
    int8_t forward = 0;

    bool operator==(MovementIntent const&) const noexcept = default;
};

struct DiscreteMovement final {
    int8_t x = 0;
    int8_t y = 0;

    bool operator==(DiscreteMovement const&) const noexcept = default;
};

class CameraController final {
public:
    [[nodiscard]]
    static DiscreteMovement cameraRelativeMovement(
        MovementIntent const intent,
        double const yaw_degrees
    ) noexcept;

private:
    [[nodiscard]]
    static int8_t sign(double const value) noexcept;
};

} // namespace client
