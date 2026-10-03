#pragma once

#include <client/PreviewResidency.hpp>

#include <shared/world/HeightTileInterest.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_set>

namespace client {

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
