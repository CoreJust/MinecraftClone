#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>

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
    explicit VulkanFrustum(glm::mat4 const& projection_view) noexcept;

    [[nodiscard]]
    bool intersects(glm::vec3 minimum, glm::vec3 maximum) const noexcept;

private:
    struct Plane final {
        glm::dvec4 coefficients{};
        glm::dvec4 error_coefficients{};
    };

    std::array<Plane, 6> m_planes;
};

} // namespace client
