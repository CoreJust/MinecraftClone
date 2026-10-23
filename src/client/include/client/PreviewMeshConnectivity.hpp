#pragma once

#include <shared/net/Message.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

namespace client {

// A non-center retained root must lie in both old and new near-camera overlap regions.
inline constexpr uint32_t PREVIEW_MESH_CONNECTIVITY_ROOT_RADIUS = 48U;

class PreviewMeshConnectivity final {
public:
    // Rebuilds after an interest change; visible keys outside the retained component must be retired.
    [[nodiscard]] std::vector<shared::HeightTileKey> rebase(
        shared::HeightTileKey center,
        uint32_t interest_radius,
        std::span<shared::HeightTileKey const> visible_keys
    );

    [[nodiscard]] bool canPublish(shared::HeightTileKey key) const;
    // Call only after the renderer has accepted the mesh upsert.
    [[nodiscard]] bool published(shared::HeightTileKey key);

    // Removes changed keys and returns the remaining visible meshes detached by that removal.
    [[nodiscard]] std::vector<shared::HeightTileKey> remove(
        std::span<shared::HeightTileKey const> removed_keys
    );

    [[nodiscard]] bool connected(shared::HeightTileKey key) const;
    [[nodiscard]] uint32_t connectedCount() const noexcept;
    void clear() noexcept;

private:
    struct KeyHash final {
        [[nodiscard]] size_t operator()(shared::HeightTileKey key) const noexcept;
    };

    using KeySet = std::unordered_set<shared::HeightTileKey, KeyHash>;

    [[nodiscard]] static std::optional<shared::HeightTileKey> nearestCurrentRoot(
        shared::HeightTileKey center,
        KeySet const& candidates
    );
    [[nodiscard]] static KeySet reachableFrom(
        shared::HeightTileKey root,
        KeySet const& candidates
    );
    [[nodiscard]] static std::vector<shared::HeightTileKey> difference(
        KeySet const& keys,
        KeySet const& retained
    );

    std::optional<shared::HeightTileKey> m_center;
    uint32_t m_interest_radius = 0U;
    KeySet m_connected;
};

} // namespace client
