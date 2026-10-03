#pragma once

#include <shared/net/Message.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace client {

struct PlayerPreviewInterestDelta final {
    std::vector<shared::HeightTileKey> additions;
    std::vector<shared::HeightTileKey> removals;
};

[[nodiscard]]
PlayerPreviewInterestDelta playerPreviewInterestDelta(
    shared::HeightTileKey previous_center,
    shared::HeightTileKey next_center,
    uint32_t radius = shared::HEIGHT_TILE_INTEREST_RADIUS
);

[[nodiscard]]
bool playerPreviewTileWithinInterest(
    shared::HeightTileKey center,
    shared::HeightTileKey key,
    uint32_t radius
);

struct PlayerPreviewLodRefresh final {
    std::vector<shared::HeightTileKey> changed_keys;
    uint32_t classified_buckets = 0U;
    uint32_t evaluated_tiles = 0U;
};

class PlayerPreviewLod final {
public:
    void clear() noexcept;
    void erase(shared::HeightTileKey key);
    [[nodiscard]]
    shared::HeightTileSurfaceDetail selectDetail(shared::HeightTileKey key, uint16_t minimum, uint16_t maximum);
    [[nodiscard]]
    std::optional<shared::HeightTileSurfaceDetail> detail(shared::HeightTileKey key) const noexcept;
    [[nodiscard]]
    PlayerPreviewLodRefresh refresh(
        shared::HeightTileKey center,
        shared::HeightTileSurfaceBounds viewer_bounds,
        shared::HeightTileSurfaceProjection projection
    );

private:
    static constexpr uint32_t BUCKET_SIDE = 4U;
    static constexpr uint32_t BUCKET_TILES = BUCKET_SIDE * BUCKET_SIDE;
    static constexpr uint32_t DETAIL_COUNT = 5U;

    struct Tile final {
        shared::HeightTileKey key;
        uint16_t minimum;
        uint16_t maximum;
        shared::HeightTileSurfaceDetail detail;
    };
    struct Group final {
        shared::HeightTileSurfaceBounds bounds;
        double minimum_height = 0.0;
        bool present = false;
    };
    struct Bucket final {
        std::array<std::optional<Tile>, BUCKET_TILES> tiles;
        std::array<Group, DETAIL_COUNT> groups;
        shared::HeightTileSurfaceBounds certified_viewer_bounds;
        uint8_t candidate_details = 0U;
        bool dirty = true;
        bool certified = false;
    };

    [[nodiscard]] static uint32_t bucketId(shared::HeightTileKey key) noexcept;
    [[nodiscard]] static uint32_t tileIndex(shared::HeightTileKey key) noexcept;
    void rebuildGroups(Bucket& bucket) const noexcept;
    [[nodiscard]] shared::HeightTileSurfaceBounds surfaceBounds(Tile const& tile) const noexcept;

    std::unordered_map<uint32_t, Bucket> m_buckets;
    shared::HeightTileKey m_center{};
    shared::HeightTileSurfaceBounds m_viewer_bounds{};
    shared::HeightTileSurfaceProjection m_projection{};
    shared::HeightTileSurfaceLodPolicy m_policy;
    bool m_initialized = false;
};

} // namespace client
