#include <client/PreviewMeshConnectivity.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/HeightTileInterest.hpp>

#include <core/common/Assert.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <deque>
#include <tuple>
#include <utility>

namespace client {

size_t PreviewMeshConnectivity::KeyHash::operator()(shared::HeightTileKey const key) const noexcept
{
    return static_cast<size_t>(shared::heightTileCoordinateHash(key.x, key.y));
}

std::vector<shared::HeightTileKey> PreviewMeshConnectivity::rebase(
    shared::HeightTileKey const center,
    uint32_t const interest_radius,
    std::span<shared::HeightTileKey const> const visible_keys
)
{
    ASSERT(shared::isValidHeightTileInterestRadius(interest_radius), "invalid preview-mesh interest radius");
    shared::HeightTileKey const normalized_center = shared::normalizeHeightTileKey(center);
    KeySet visible;
    KeySet eligible;
    visible.reserve(visible_keys.size());
    eligible.reserve(visible_keys.size());
    for (shared::HeightTileKey const raw_key : visible_keys) {
        shared::HeightTileKey const key = shared::normalizeHeightTileKey(raw_key);
        visible.insert(key);
        if (shared::heightTileWithinInterest(normalized_center, key, interest_radius)) {
            eligible.insert(key);
        }
    }

    std::optional<shared::HeightTileKey> root;
    if (eligible.contains(normalized_center)) {
        root = normalized_center;
    }
    if (!root && m_center.has_value()) {
        KeySet overlap;
        overlap.reserve(m_connected.size());
        for (shared::HeightTileKey const key : m_connected) {
            if (eligible.contains(key)
                && shared::heightTileWithinInterest(*m_center, key, PREVIEW_MESH_CONNECTIVITY_ROOT_RADIUS)
                && shared::heightTileWithinInterest(
                    normalized_center, key, PREVIEW_MESH_CONNECTIVITY_ROOT_RADIUS
                )) {
                overlap.insert(key);
            }
        }
        root = nearestCurrentRoot(normalized_center, overlap);
    }

    KeySet connected;
    if (root.has_value()) {
        connected = reachableFrom(*root, eligible);
    }
    std::vector<shared::HeightTileKey> detached = difference(visible, connected);
    m_center = normalized_center;
    m_interest_radius = interest_radius;
    m_connected = std::move(connected);
    return detached;
}

bool PreviewMeshConnectivity::canPublish(shared::HeightTileKey const raw_key) const
{
    if (!m_center.has_value()) {
        return false;
    }
    shared::HeightTileKey const key = shared::normalizeHeightTileKey(raw_key);
    if (!shared::heightTileWithinInterest(*m_center, key, m_interest_radius)) {
        return false;
    }
    if (m_connected.contains(key)) {
        return true;
    }
    if (m_connected.empty()) {
        return key == *m_center;
    }

    std::array const neighbors{
        shared::normalizeHeightTileKey({ key.x - 1, key.y }),
        shared::normalizeHeightTileKey({ key.x + 1, key.y }),
        shared::normalizeHeightTileKey({ key.x, key.y - 1 }),
        shared::normalizeHeightTileKey({ key.x, key.y + 1 }),
    };
    return std::any_of(neighbors.begin(), neighbors.end(), [this](shared::HeightTileKey const neighbor) {
        return m_connected.contains(neighbor);
    });
}

bool PreviewMeshConnectivity::published(shared::HeightTileKey const key)
{
    if (!canPublish(key)) {
        return false;
    }
    m_connected.insert(shared::normalizeHeightTileKey(key));
    return true;
}

std::vector<shared::HeightTileKey> PreviewMeshConnectivity::remove(
    std::span<shared::HeightTileKey const> const removed_keys
)
{
    KeySet previous = std::move(m_connected);
    for (shared::HeightTileKey const raw_key : removed_keys) {
        previous.erase(shared::normalizeHeightTileKey(raw_key));
    }

    std::optional<shared::HeightTileKey> root;
    if (m_center.has_value() && previous.contains(*m_center)) {
        root = *m_center;
    } else if (m_center.has_value()) {
        root = nearestCurrentRoot(*m_center, previous);
    }
    if (root.has_value()) {
        m_connected = reachableFrom(*root, previous);
    } else {
        m_connected.clear();
    }
    return difference(previous, m_connected);
}

bool PreviewMeshConnectivity::connected(shared::HeightTileKey const key) const
{
    return m_connected.contains(shared::normalizeHeightTileKey(key));
}

uint32_t PreviewMeshConnectivity::connectedCount() const noexcept
{
    return static_cast<uint32_t>(m_connected.size());
}

void PreviewMeshConnectivity::clear() noexcept
{
    m_center.reset();
    m_interest_radius = 0U;
    m_connected.clear();
}

std::optional<shared::HeightTileKey> PreviewMeshConnectivity::nearestCurrentRoot(
    shared::HeightTileKey const center,
    KeySet const& candidates
)
{
    std::optional<shared::HeightTileKey> closest;
    double closest_priority = 0.0;
    for (shared::HeightTileKey const key : candidates) {
        if (!shared::heightTileWithinInterest(center, key, PREVIEW_MESH_CONNECTIVITY_ROOT_RADIUS)) {
            continue;
        }
        double const priority = shared::heightTileInterestPriority(center, 0, 127, key);
        if (!closest.has_value() || std::tie(priority, key.y, key.x)
                < std::tie(closest_priority, closest->y, closest->x)) {
            closest = key;
            closest_priority = priority;
        }
    }
    return closest;
}

PreviewMeshConnectivity::KeySet PreviewMeshConnectivity::reachableFrom(
    shared::HeightTileKey const root,
    KeySet const& candidates
)
{
    KeySet connected;
    if (!candidates.contains(root)) {
        return connected;
    }
    connected.reserve(candidates.size());
    std::deque<shared::HeightTileKey> frontier;
    connected.insert(root);
    frontier.push_back(root);
    while (!frontier.empty()) {
        shared::HeightTileKey const key = frontier.front();
        frontier.pop_front();
        std::array const neighbors{
            shared::normalizeHeightTileKey({ key.x - 1, key.y }),
            shared::normalizeHeightTileKey({ key.x + 1, key.y }),
            shared::normalizeHeightTileKey({ key.x, key.y - 1 }),
            shared::normalizeHeightTileKey({ key.x, key.y + 1 }),
        };
        for (shared::HeightTileKey const neighbor : neighbors) {
            if (candidates.contains(neighbor) && connected.insert(neighbor).second) {
                frontier.push_back(neighbor);
            }
        }
    }
    return connected;
}

std::vector<shared::HeightTileKey> PreviewMeshConnectivity::difference(
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    KeySet const& keys,
    KeySet const& retained
)
{
    std::vector<shared::HeightTileKey> result;
    result.reserve(keys.size());
    for (shared::HeightTileKey const key : keys) {
        if (!retained.contains(key)) {
            result.push_back(key);
        }
    }
    std::sort(result.begin(), result.end(), [](shared::HeightTileKey const lhs, shared::HeightTileKey const rhs) {
        return std::tie(lhs.y, lhs.x) < std::tie(rhs.y, rhs.x);
    });
    return result;
}

} // namespace client
