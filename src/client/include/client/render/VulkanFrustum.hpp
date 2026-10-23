#pragma once

#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/vector_double3.hpp>
#include <glm/ext/vector_double4.hpp>
#include <glm/ext/vector_float3.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>

namespace client {

struct WrappedBounds final {
    glm::vec3 minimum{};
    glm::vec3 maximum{};
};

[[nodiscard]]
WrappedBounds boundsNearestToCamera(
    glm::vec3 minimum,
    glm::vec3 maximum,
    glm::dvec3 camera_position
) noexcept;

inline constexpr float WORLD_CURVATURE_RADIUS = 131'072.0F;
inline constexpr float MIN_WORLD_VISIBILITY_RADIUS = 16'384.0F;
inline constexpr float MAX_WORLD_VISIBILITY_RADIUS = 32'768.0F;
inline constexpr double WORLD_VISIBILITY_SEA_LEVEL = 384.0;

[[nodiscard]]
float worldVisibilityRadius(double camera_world_z) noexcept;

[[nodiscard]]
double curvedWorldFarPlane(double camera_altitude, double current_far_plane) noexcept;

[[nodiscard]]
glm::vec3 curvedWorldPosition(glm::vec3 camera_relative_position) noexcept;

[[nodiscard]]
WrappedBounds curvedBoundsNearestToCamera(
    glm::vec3 minimum,
    glm::vec3 maximum,
    glm::dvec3 camera_position
) noexcept;

[[nodiscard]]
bool insideWorldVisibility(WrappedBounds const& camera_relative_bounds, float visibility_radius) noexcept;

class VulkanFrustum final {
public:
    static constexpr uint8_t ALL_PLANES = 63U;

    struct Classification final {
        uint8_t plane_mask = ALL_PLANES;
        uint32_t plane_tests = 0U;
        bool outside = false;
    };

    explicit VulkanFrustum(glm::mat4 const& projection_view) noexcept;

    [[nodiscard]]
    bool intersects(glm::vec3 minimum, glm::vec3 maximum) const noexcept;

    [[nodiscard]]
    bool intersects(
        glm::vec3 minimum,
        glm::vec3 maximum,
        uint8_t plane_mask,
        uint32_t& plane_tests
    ) const noexcept;

    [[nodiscard]]
    Classification classify(glm::vec3 minimum, glm::vec3 maximum) const noexcept;

    [[nodiscard]]
    bool hasSamePlanes(VulkanFrustum const& other) const noexcept;

private:
    struct Plane final {
        glm::dvec4 coefficients{};
        glm::dvec4 error_coefficients{};
    };

    std::array<Plane, 6> m_planes;
};

} // namespace client
