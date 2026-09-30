#pragma once

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

private:
    struct Plane final {
        glm::dvec4 coefficients{};
        glm::dvec4 error_coefficients{};
    };

    std::array<Plane, 6> m_planes;
};

} // namespace client
