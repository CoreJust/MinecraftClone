#pragma once

#include <client/PreviewResidency.hpp>

#include <shared/world/HeightTileInterest.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <tuple>
#include <unordered_set>

namespace client {

inline constexpr uint8_t PREVIEW_MESH_WORK_TIER_COUNT = 6U;

[[nodiscard]] inline uint8_t previewMeshWorkTier(uint8_t const geographic_tier, bool const missing_current_mesh)
{
    return geographic_tier < 2U ? geographic_tier
        : static_cast<uint8_t>(geographic_tier + (missing_current_mesh ? 0U : 2U));
}

template<typename WorkRange, typename PriorityTier>
[[nodiscard]] auto lowestPriorityPreviewMeshAbove(
    WorkRange& work, PriorityTier priority_tier, uint8_t const admitted_tier
)
{
    auto candidate = work.end();
    uint8_t deferred_tier = admitted_tier;
    for (auto item = work.begin(); item != work.end(); ++item) {
        uint8_t const tier = previewMeshWorkTier(priority_tier(item->key), item->missing_current_mesh);
        if (tier > deferred_tier) {
            candidate = item;
            deferred_tier = tier;
        }
    }
    return candidate;
}

template<typename WorkRange, typename PriorityTier>
[[nodiscard]] std::optional<shared::HeightTileKey> deferLowerPriorityPreviewMesh(
    WorkRange& work,
    PriorityTier priority_tier,
    uint8_t const admitted_tier
)
{
    auto candidate = work.end();
    uint8_t deferred_tier = admitted_tier;
    for (auto item = work.begin(); item != work.end(); ++item) {
        uint8_t const tier = priority_tier(item->key);
        if (tier > deferred_tier) {
            candidate = item;
            deferred_tier = tier;
        }
    }
    if (candidate == work.end()) {
        return std::nullopt;
    }
    shared::HeightTileKey const key = candidate->key;
    work.erase(candidate);
    return key;
}

template<typename PriorityTier>
[[nodiscard]] auto previewMeshWorkPriority(
    shared::HeightTileKey const center,
    int8_t const heading_x,
    int8_t const heading_y,
    shared::HeightTileKey const key,
    PriorityTier priority_tier,
    bool const missing_current_mesh = false
)
{
    return std::tuple{
        previewMeshWorkTier(priority_tier(key), missing_current_mesh),
        shared::heightTileInterestPriority(center, heading_x, heading_y, key),
        key.y,
        key.x,
    };
}

template<typename ResultRange, typename PriorityTier>
[[nodiscard]] std::optional<uint32_t> preferredPreviewMeshResultIndex(
    ResultRange const& results,
    shared::HeightTileKey const center,
    int8_t const heading_x,
    int8_t const heading_y,
    PriorityTier priority_tier
)
{
    std::optional<uint32_t> preferred;
    decltype(previewMeshWorkPriority(center, heading_x, heading_y, shared::HeightTileKey{}, priority_tier))
        preferred_priority{};
    uint32_t index = 0U;
    for (auto const& result : results) {
        auto const priority = previewMeshWorkPriority(
            center, heading_x, heading_y, result.key, priority_tier, result.missing_current_mesh
        );
        if (!preferred.has_value() || priority < preferred_priority) {
            preferred = index;
            preferred_priority = priority;
        }
        ++index;
    }
    return preferred;
}

template<typename ResultRange>
[[nodiscard]] std::optional<uint32_t> preferredPreviewMeshResultIndex(
    ResultRange const& results,
    shared::HeightTileKey const center,
    int8_t const heading_x,
    int8_t const heading_y
)
{
    return preferredPreviewMeshResultIndex(
        results, center, heading_x, heading_y, [](shared::HeightTileKey) { return uint8_t{0U}; }
    );
}

template<typename HandleChange>
void drainPreviewTileChanges(
    PreviewResidency& residency,
    uint32_t const maximum_changes,
    bool const interest_initialized,
    HandleChange handle_change
)
{
    if (!interest_initialized) {
        return;
    }
    for (HeightTileChange const change : residency.takeChanges(maximum_changes)) {
        handle_change(change);
    }
}

class PreviewMeshWorkQueue final {
public:
    void enqueue(HeightTileKey const key)
    {
        HeightTileKey const normalized = shared::normalizeHeightTileKey(key);
        if (m_queued.insert(normalized).second) {
            m_keys.push_back(normalized);
        }
    }

    void enqueueChange(HeightTileChange const change)
    {
        enqueueChange(change, [](HeightTileKey) { return true; });
    }

    template<typename ShouldEnqueue>
    void enqueueChange(HeightTileChange const change, ShouldEnqueue should_enqueue)
    {
        std::array<HeightTileKey, 5> const candidates{
            change.key,
            { change.key.x - 1, change.key.y },
            { change.key.x + 1, change.key.y },
            { change.key.x, change.key.y - 1 },
            { change.key.x, change.key.y + 1 },
        };
        for (HeightTileKey const candidate : candidates) {
            HeightTileKey const normalized = shared::normalizeHeightTileKey(candidate);
            if (should_enqueue(normalized)) {
                enqueue(normalized);
            }
        }
    }

    [[nodiscard]] std::optional<HeightTileKey> take()
    {
        if (m_keys.empty()) {
            return std::nullopt;
        }
        HeightTileKey const key = m_keys.front();
        m_keys.pop_front();
        m_queued.erase(key);
        return key;
    }

    [[nodiscard]] bool empty() const noexcept { return m_keys.empty(); }
    [[nodiscard]] uint32_t size() const noexcept { return static_cast<uint32_t>(m_keys.size()); }

    template<typename ShouldRetain>
    void retain(ShouldRetain should_retain)
    {
        std::deque<HeightTileKey> retained;
        std::unordered_set<HeightTileKey, KeyHash> retained_keys;
        retained_keys.reserve(m_queued.size());
        for (HeightTileKey const key : m_keys) {
            if (should_retain(key) && retained_keys.insert(key).second) {
                retained.push_back(key);
            }
        }
        m_keys.swap(retained);
        m_queued.swap(retained_keys);
    }

    void clear() noexcept
    {
        m_keys.clear();
        m_queued.clear();
    }

private:
    struct KeyHash final {
        [[nodiscard]] uint64_t operator()(HeightTileKey const key) const noexcept
        {
            return shared::heightTileCoordinateHash(key.x, key.y);
        }
    };

    std::deque<HeightTileKey> m_keys;
    std::unordered_set<HeightTileKey, KeyHash> m_queued;
};

} // namespace client
