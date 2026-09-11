#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>

namespace client {

struct CameraAngles final {
    double yaw_degrees = 0.0;
    double pitch_degrees = 0.0;
    double roll_degrees = 0.0;

    bool operator==(CameraAngles const&) const noexcept = default;
};

struct CameraPose final {
    glm::dvec3 position{ 0.0, 0.0, 0.0 };
    CameraAngles angles{ };

    bool operator==(CameraPose const&) const noexcept = default;
};

struct CameraProjection final {
    double vertical_fov_degrees = 70.0;
    double near_plane = 0.1;
    double far_plane = 1'000.0;

    bool operator==(CameraProjection const&) const noexcept = default;
};

class Camera final {
public:
    static constexpr double MIN_PITCH_DEGREES = -89.0;
    static constexpr double MAX_PITCH_DEGREES = 89.0;

    explicit Camera(
        CameraPose pose = { },
        CameraProjection projection = { }
    ) noexcept;

    [[nodiscard]]
    CameraPose pose() const noexcept;
    [[nodiscard]]
    CameraProjection projection() const noexcept;

    [[nodiscard]]
    bool setPosition(glm::dvec3 const position) noexcept;
    [[nodiscard]]
    bool setAngles(CameraAngles const angles) noexcept;
    [[nodiscard]]
    bool rotate(
        double yaw_delta_degrees,
        double pitch_delta_degrees,
        double roll_delta_degrees = 0.0
    ) noexcept;
    [[nodiscard]]
    bool setProjection(CameraProjection const projection) noexcept;

    [[nodiscard]]
    glm::dvec3 forward() const noexcept;
    [[nodiscard]]
    glm::dvec3 right() const noexcept;
    [[nodiscard]]
    glm::dvec3 up() const noexcept;
    [[nodiscard]]
    glm::mat4 viewMatrix() const noexcept;

    [[nodiscard]]
    std::optional<glm::mat4> projectionMatrix(uint32_t const width, uint32_t const height) const noexcept;

private:
    static double normalizeYaw(double yaw_degrees) noexcept;
    static double clampPitch(double pitch_degrees) noexcept;

    glm::dvec3 m_position{ 0.0, 0.0, 0.0 };
    double m_yaw_radians = 0.0;
    double m_pitch_radians = 0.0;
    double m_roll_radians = 0.0;
    CameraProjection m_projection{ };
};

} // namespace client
