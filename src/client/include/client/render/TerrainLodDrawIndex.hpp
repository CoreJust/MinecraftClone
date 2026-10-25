#pragma once

#include <client/TerrainLodPresentation.hpp>
#include <client/render/VulkanFrustum.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

namespace client {

// Immutable scene bounds; GPU allocation ranges remain owned by the renderer.
class TerrainLodDrawIndex final {
public:
    struct Work final {
        uint32_t nodes_tested = 0U;
        uint32_t groups_tested = 0U;
    };

    void reset(std::span<TerrainLodDrawGroup const> groups)
    {
        m_bounds.clear();
        m_nodes.clear();
        m_order.resize(groups.size());
        std::iota(m_order.begin(), m_order.end(), 0U);
        m_bounds.reserve(groups.size());
        for (auto const& group : groups) {
            m_bounds.push_back({
                { static_cast<float>(group.origin_x), static_cast<float>(group.origin_y),
                    static_cast<float>(group.minimum_z) },
                { static_cast<float>(group.origin_x + group.extent),
                    static_cast<float>(group.origin_y + group.extent), static_cast<float>(group.maximum_z) },
            });
        }
        if (!groups.empty()) {
            build(0U, static_cast<uint32_t>(groups.size()));
        }
    }

    [[nodiscard]]
    Work collect(VulkanFrustum const& frustum, glm::dvec3 camera, std::vector<uint32_t>& visible) const
    {
        visible.clear();
        Work work;
        if (!m_nodes.empty()) {
            visit(0U, frustum, camera, worldVisibilityRadius(camera.z), visible, work);
        }
        // Traversal is spatial; publication and range coalescing retain scene order.
        std::sort(visible.begin(), visible.end());
        return work;
    }

private:
    struct Node final {
        WrappedBounds bounds;
        glm::vec2 minimum_center{};
        glm::vec2 maximum_center{};
        uint32_t begin = 0U;
        uint32_t end = 0U;
        uint32_t right = 0U;
    };

    uint32_t build(uint32_t begin, uint32_t end)
    {
        uint32_t const result = static_cast<uint32_t>(m_nodes.size());
        Node node{ .bounds = m_bounds[m_order[begin]], .begin = begin, .end = end };
        node.minimum_center = node.maximum_center = glm::vec2(
            (node.bounds.minimum + node.bounds.maximum) * 0.5F);
        for (uint32_t i = begin + 1U; i < end; ++i) {
            auto const& bounds = m_bounds[m_order[i]];
            node.bounds.minimum = glm::min(node.bounds.minimum, bounds.minimum);
            node.bounds.maximum = glm::max(node.bounds.maximum, bounds.maximum);
            glm::vec2 const center = glm::vec2((bounds.minimum + bounds.maximum) * 0.5F);
            node.minimum_center = glm::min(node.minimum_center, center);
            node.maximum_center = glm::max(node.maximum_center, center);
        }
        m_nodes.push_back(node);
        if (end - begin > 8U) {
            int const axis = node.maximum_center.x - node.minimum_center.x
                >= node.maximum_center.y - node.minimum_center.y ? 0 : 1;
            uint32_t const middle = begin + (end - begin) / 2U;
            std::nth_element(m_order.begin() + begin, m_order.begin() + middle, m_order.begin() + end,
                [&](uint32_t a, uint32_t b) {
                    float const ca = m_bounds[a].minimum[axis] + m_bounds[a].maximum[axis];
                    float const cb = m_bounds[b].minimum[axis] + m_bounds[b].maximum[axis];
                    return ca == cb ? a < b : ca < cb;
                });
            build(begin, middle);
            m_nodes[result].right = build(middle, end);
        }
        return result;
    }

    static double shift(double camera, float center) noexcept
    {
        return std::round((camera - static_cast<double>(center)) / 65'536.0) * 65'536.0;
    }

    void visit(uint32_t ordinal, VulkanFrustum const& frustum, glm::dvec3 camera, float radius,
        std::vector<uint32_t>& visible, Work& work) const
    {
        Node const& node = m_nodes[ordinal];
        ++work.nodes_tested;
        bool same_image = true;
        glm::vec3 const center = (node.bounds.minimum + node.bounds.maximum) * 0.5F;
        for (int axis = 0; axis < 2; ++axis) {
            double const image = shift(camera[axis], center[axis]);
            same_image = same_image && image == shift(camera[axis], node.minimum_center[axis])
                && image == shift(camera[axis], node.maximum_center[axis]);
        }
        // A nearest-image cut through descendants has no single conservative wrapped envelope.
        if (same_image) {
            auto const bounds = curvedBoundsNearestToCamera(node.bounds.minimum, node.bounds.maximum, camera);
            if (!insideWorldVisibility(bounds, radius) || !frustum.intersects(bounds.minimum, bounds.maximum)) {
                return;
            }
        }
        if (node.right != 0U) {
            visit(ordinal + 1U, frustum, camera, radius, visible, work);
            visit(node.right, frustum, camera, radius, visible, work);
            return;
        }
        for (uint32_t i = node.begin; i < node.end; ++i) {
            ++work.groups_tested;
            uint32_t const group = m_order[i];
            auto const bounds = curvedBoundsNearestToCamera(m_bounds[group].minimum, m_bounds[group].maximum, camera);
            if (insideWorldVisibility(bounds, radius) && frustum.intersects(bounds.minimum, bounds.maximum)) {
                visible.push_back(group);
            }
        }
    }

    std::vector<WrappedBounds> m_bounds;
    std::vector<uint32_t> m_order;
    std::vector<Node> m_nodes;
};

} // namespace client
