#include <client/Camera.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace client {

namespace {

constexpr double DEGREES_TO_RADIANS = 0.017'453'292'519'943'295'769'236'907'684'89;
constexpr double RADIANS_TO_DEGREES = 57.295'779'513'082'320'876'798'154'814'105;
constexpr double FULL_ROTATION_DEGREES = 360.0;

bool isFinite(glm::dvec3 const value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool isValidProjection(CameraProjection const projection) noexcept {
    return std::isfinite(projection.vertical_fov_degrees)
        && std::isfinite(projection.near_plane)
        && std::isfinite(projection.far_plane)
        && projection.vertical_fov_degrees > 0.0
        && projection.vertical_fov_degrees < 180.0
        && projection.near_plane > 0.0
        && projection.far_plane > projection.near_plane;
}

} // namespace

Camera::Camera(CameraPose const pose, CameraProjection const projection) noexcept
    : m_projection(isValidProjection(projection) ? projection : CameraProjection{ })
{
    if (isFinite(pose.position)) {
        m_position = pose.position;
    }
    if (std::isfinite(pose.angles.yaw_degrees)) {
        m_yaw_radians = normalizeYaw(pose.angles.yaw_degrees) * DEGREES_TO_RADIANS;
    }
    if (std::isfinite(pose.angles.pitch_degrees)) {
        m_pitch_radians = clampPitch(pose.angles.pitch_degrees) * DEGREES_TO_RADIANS;
    }
    if (std::isfinite(pose.angles.roll_degrees)) {
        m_roll_radians = normalizeYaw(pose.angles.roll_degrees) * DEGREES_TO_RADIANS;
    }
}

CameraPose Camera::pose() const noexcept {
    return CameraPose{
        .position = m_position,
        .angles = CameraAngles{
            .yaw_degrees = normalizeYaw(m_yaw_radians * RADIANS_TO_DEGREES),
            .pitch_degrees = m_pitch_radians * RADIANS_TO_DEGREES,
            .roll_degrees = normalizeYaw(m_roll_radians * RADIANS_TO_DEGREES),
        },
    };
}

CameraProjection Camera::projection() const noexcept {
    return m_projection;
}

bool Camera::setPosition(glm::dvec3 const position) noexcept {
    if (!isFinite(position)) {
        return false;
    }
    m_position = position;
    return true;
}

bool Camera::setAngles(CameraAngles const angles) noexcept {
    if (!std::isfinite(angles.yaw_degrees) || !std::isfinite(angles.pitch_degrees)
        || !std::isfinite(angles.roll_degrees)) {
        return false;
    }
    m_yaw_radians = normalizeYaw(angles.yaw_degrees) * DEGREES_TO_RADIANS;
    m_pitch_radians = clampPitch(angles.pitch_degrees) * DEGREES_TO_RADIANS;
    m_roll_radians = normalizeYaw(angles.roll_degrees) * DEGREES_TO_RADIANS;
    return true;
}

bool Camera::rotate(
    double const yaw_delta_degrees,
    double const pitch_delta_degrees,
    double const roll_delta_degrees
) noexcept {
    if (!std::isfinite(yaw_delta_degrees) || !std::isfinite(pitch_delta_degrees)
        || !std::isfinite(roll_delta_degrees)) {
        return false;
    }
    double const current_yaw_degrees = m_yaw_radians * RADIANS_TO_DEGREES;
    double const current_pitch_degrees = m_pitch_radians * RADIANS_TO_DEGREES;
    double const next_yaw_degrees = current_yaw_degrees
        + std::fmod(yaw_delta_degrees, FULL_ROTATION_DEGREES);
    double next_pitch_degrees = current_pitch_degrees;
    if (pitch_delta_degrees > MAX_PITCH_DEGREES - current_pitch_degrees) {
        next_pitch_degrees = MAX_PITCH_DEGREES;
    } else if (pitch_delta_degrees < MIN_PITCH_DEGREES - current_pitch_degrees) {
        next_pitch_degrees = MIN_PITCH_DEGREES;
    } else {
        next_pitch_degrees += pitch_delta_degrees;
    }
    m_yaw_radians = normalizeYaw(next_yaw_degrees) * DEGREES_TO_RADIANS;
    m_pitch_radians = clampPitch(next_pitch_degrees) * DEGREES_TO_RADIANS;
    m_roll_radians = normalizeYaw(
        m_roll_radians * RADIANS_TO_DEGREES + std::fmod(roll_delta_degrees, FULL_ROTATION_DEGREES)
    ) * DEGREES_TO_RADIANS;
    return true;
}

bool Camera::setProjection(CameraProjection const projection) noexcept {
    if (!isValidProjection(projection)) {
        return false;
    }
    m_projection = projection;
    return true;
}

glm::dvec3 Camera::forward() const noexcept {
    double const cosine_pitch = std::cos(m_pitch_radians);
    return glm::dvec3{
        std::sin(m_yaw_radians) * cosine_pitch,
        std::cos(m_yaw_radians) * cosine_pitch,
        std::sin(m_pitch_radians),
    };
}

glm::dvec3 Camera::right() const noexcept {
    return glm::dvec3{
        std::cos(m_yaw_radians),
        -std::sin(m_yaw_radians),
        0.0,
    };
}

glm::dvec3 Camera::up() const noexcept {
    glm::dvec3 const unrolled_up = glm::normalize(glm::cross(right(), forward()));
    return unrolled_up * std::cos(m_roll_radians)
        + glm::cross(forward(), unrolled_up) * std::sin(m_roll_radians);
}

glm::mat4 Camera::viewMatrix() const noexcept {
    glm::dvec3 const direction = forward();
    glm::dmat4 const view = glm::lookAtRH(m_position, m_position + direction, up());
    return glm::mat4(view);
}

std::optional<glm::mat4> Camera::projectionMatrix(uint32_t const width, uint32_t const height) const noexcept {
    if (width == 0 || height == 0) {
        return std::nullopt;
    }
    double const aspect = static_cast<double>(width) / static_cast<double>(height);
    glm::mat4 projection = glm::mat4(glm::perspectiveRH_ZO(
        m_projection.vertical_fov_degrees * DEGREES_TO_RADIANS,
        aspect,
        m_projection.near_plane,
        m_projection.far_plane
    ));
    projection[1][1] = -projection[1][1];
    return projection;
}

double Camera::normalizeYaw(double yaw_degrees) noexcept {
    yaw_degrees = std::fmod(yaw_degrees, FULL_ROTATION_DEGREES);
    if (yaw_degrees < 0.0) {
        yaw_degrees += FULL_ROTATION_DEGREES;
    }
    return yaw_degrees == 0.0 ? 0.0 : yaw_degrees;
}

double Camera::clampPitch(double pitch_degrees) noexcept {
    if (pitch_degrees < MIN_PITCH_DEGREES) {
        return MIN_PITCH_DEGREES;
    }
    if (pitch_degrees > MAX_PITCH_DEGREES) {
        return MAX_PITCH_DEGREES;
    }
    return pitch_degrees;
}

} // namespace client
