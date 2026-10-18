#pragma once

#include <shared/net/Message.hpp>
#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace client {

using PlayerPreviewInterestDelta = shared::HeightTileInterestDelta;

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

[[nodiscard]]
std::vector<shared::HeightTileKey> playerPreviewLodRemeshClosure(
    std::span<shared::HeightTileKey const> changed_keys
);

void playerPreviewLodRemeshClosure(
    std::span<shared::HeightTileKey const> changed_keys,
    std::vector<shared::HeightTileKey>& output
);

struct PlayerPreviewLodRefresh final {
    std::vector<shared::HeightTileKey> changed_keys;
    uint32_t visited_buckets = 0U;
    uint32_t classified_buckets = 0U;
    uint32_t evaluated_tiles = 0U;
    uint32_t policy_evaluations = 0U;
    uint32_t optional_buckets_inspected = 0U;
    bool full_scan = false;
};

class PlayerPreviewLod final {
public:
    void clear() noexcept;
    void erase(shared::HeightTileKey key);
    void eraseBatch(std::span<shared::HeightTileKey const> keys);
    [[nodiscard]]
    shared::HeightTileSurfaceDetail selectDetail(shared::HeightTileKey key, uint16_t minimum, uint16_t maximum);
    [[nodiscard]]
    shared::HeightTileSurfaceDetail selectDetail(
        shared::HeightTileKey key,
        uint16_t minimum,
        uint16_t maximum,
        shared::HeightTileSurfaceFaceAreas const& face_areas
    );
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
    static constexpr uint32_t MAX_OPTIONAL_FINE_BUCKETS_PER_REFRESH = 64U;

    struct Tile final {
        shared::HeightTileKey key;
        uint16_t minimum;
        uint16_t maximum;
        shared::HeightTileSurfaceFaceAreas face_areas;
        shared::HeightTileSurfaceDetail detail;
    };
    struct Group final {
        shared::HeightTileSurfaceBounds bounds;
        shared::HeightTileSurfaceFaceAreas minimum_areas;
        shared::HeightTileSurfaceFaceAreas maximum_areas;
        bool present = false;
    };
    struct Bucket final {
        std::array<std::optional<Tile>, BUCKET_TILES> tiles;
        std::array<Group, DETAIL_COUNT> groups;
        shared::HeightTileSurfaceBounds certified_viewer_bounds;
        shared::HeightTileKey certified_center{};
        uint8_t candidate_details = 0U;
        uint32_t resident_tile_count = 0U;
        uint32_t non_fine_tile_count = 0U;
        uint64_t certificate_epoch = 0U;
        uint64_t optional_sweep_epoch = 0U;
        bool dirty = true;
        bool certified = false;
    };

    [[nodiscard]] static uint32_t bucketId(shared::HeightTileKey key) noexcept;
    [[nodiscard]] static uint32_t tileIndex(shared::HeightTileKey key) noexcept;
    [[nodiscard]] static bool imageTranslationStable(
        Bucket const& bucket,
        shared::HeightTileKey current_center,
        double center_offset_x,
        double center_offset_y
    ) noexcept;
    [[nodiscard]] bool centerTranslationStable(shared::HeightTileKey current_center) const noexcept;
    void addCertifiedOffset(shared::HeightTileKey key);
    void removeCertifiedOffset(shared::HeightTileKey key);
    void updateBucketIndexes(uint32_t id, Bucket& bucket);
    void requestOptionalFineSweep() noexcept;
    void rebuildGroups(Bucket& bucket) const noexcept;
    [[nodiscard]] shared::HeightTileSurfaceBounds surfaceBounds(Tile const& tile) const noexcept;

    std::unordered_map<uint32_t, Bucket> m_buckets;
    std::unordered_set<uint32_t> m_candidate_buckets;
    std::unordered_set<uint32_t> m_dirty_buckets;
    std::unordered_set<uint32_t> m_mandatory_buckets;
    std::set<uint32_t> m_fine_buckets;
    shared::HeightTileKey m_center{};
    shared::HeightTileKey m_certified_center{};
    shared::HeightTileSurfaceBounds m_viewer_bounds{};
    shared::HeightTileSurfaceBounds m_certified_viewer_bounds{};
    std::map<int32_t, uint32_t> m_resident_x_coordinates;
    std::map<int32_t, uint32_t> m_resident_y_coordinates;
    shared::HeightTileSurfaceProjection m_projection{};
    shared::HeightTileSurfaceLodPolicy m_policy;
    uint64_t m_certificate_epoch = 1U;
    uint64_t m_optional_sweep_epoch = 1U;
    uint32_t m_optional_cursor_id = 0U;
    bool m_optional_cursor_initialized = false;
    bool m_optional_fine_sweep_active = false;
    bool m_initialized = false;
};

} // namespace client
