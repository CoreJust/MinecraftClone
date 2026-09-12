#pragma once

#include <cstdint>

namespace client {

struct MovementIntent final {
    int8_t strafe = 0;
    int8_t forward = 0;

    bool operator==(MovementIntent const&) const noexcept = default;
};

struct MovementDirection final {
    int8_t x = 0;
    int8_t y = 0;

    bool operator==(MovementDirection const&) const noexcept = default;
};

class CameraController final {
public:
    [[nodiscard]]
    static MovementDirection cameraRelativeMovement(
        MovementIntent const intent,
        double const yaw_degrees
    ) noexcept;

private:
    [[nodiscard]]
    static int8_t quantize(double value) noexcept;
};

} // namespace client
