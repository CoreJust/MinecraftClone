#include <client/render/VulkanFrustum.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

namespace client {

namespace {

constexpr double WORLD_WRAP_PERIOD = 65'536.0;
constexpr double CLIP_ROUNDING_TOLERANCE = 8.0 * std::numeric_limits<float>::epsilon();

} // namespace

WrappedBounds boundsNearestToCamera(
    glm::vec3 const minimum,
    glm::vec3 const maximum,
    glm::dvec3 const camera_position
) noexcept {
    glm::vec3 const center = (minimum + maximum) * 0.5F;
    float const x_shift = static_cast<float>(
        std::round((camera_position.x - static_cast<double>(center.x)) / WORLD_WRAP_PERIOD)
        * WORLD_WRAP_PERIOD
    );
    float const y_shift = static_cast<float>(
        std::round((camera_position.y - static_cast<double>(center.y)) / WORLD_WRAP_PERIOD)
        * WORLD_WRAP_PERIOD
    );
    return {
        .minimum = { minimum.x + x_shift, minimum.y + y_shift, minimum.z },
        .maximum = { maximum.x + x_shift, maximum.y + y_shift, maximum.z },
    };
}

VulkanFrustum::VulkanFrustum(glm::mat4 const& projection_view) noexcept
{
    std::array<glm::dvec4, 4> rows;
    for (uint32_t row = 0U; row < rows.size(); ++row) {
        int32_t const matrix_row = static_cast<int32_t>(row);
        rows[row] = {
            projection_view[0][matrix_row],
            projection_view[1][matrix_row],
            projection_view[2][matrix_row],
            projection_view[3][matrix_row],
        };
    }
    m_planes = {
        Plane{ rows[3] + rows[0], glm::abs(rows[3]) + glm::abs(rows[0]) },
        Plane{ rows[3] - rows[0], glm::abs(rows[3]) + glm::abs(rows[0]) },
        Plane{ rows[3] + rows[1], glm::abs(rows[3]) + glm::abs(rows[1]) },
        Plane{ rows[3] - rows[1], glm::abs(rows[3]) + glm::abs(rows[1]) },
        Plane{ rows[2], glm::abs(rows[2]) },
        Plane{ rows[3] - rows[2], glm::abs(rows[3]) + glm::abs(rows[2]) },
    };
}

bool VulkanFrustum::intersects(glm::vec3 const minimum, glm::vec3 const maximum) const noexcept
{
    glm::dvec4 const maximum_magnitude{ glm::max(glm::abs(minimum), glm::abs(maximum)), 1.0 };
    for (Plane const& plane : m_planes) {
        glm::dvec4 const support{
            plane.coefficients.x >= 0.0 ? maximum.x : minimum.x,
            plane.coefficients.y >= 0.0 ? maximum.y : minimum.y,
            plane.coefficients.z >= 0.0 ? maximum.z : minimum.z,
            1.0,
        };
        double const signed_distance = glm::dot(plane.coefficients, support);
        // Keep boxes touching a clip plane despite rounding in the original float matrix products.
        double const rounding_bound = CLIP_ROUNDING_TOLERANCE
            * glm::dot(plane.error_coefficients, maximum_magnitude);
        if (signed_distance < -rounding_bound) {
            return false;
        }
    }
    return true;
}

} // namespace client
