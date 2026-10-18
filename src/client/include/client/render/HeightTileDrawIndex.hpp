#pragma once

#include <client/render/HeightTileInstanceReaper.hpp>
#include <client/render/StoneIndirectDraws.hpp>
#include <client/render/VulkanFrustum.hpp>

#include <shared/world/WorldGeneration.hpp>

#include <glm/ext/vector_double3.hpp>
#include <glm/ext/vector_float2.hpp>
#include <glm/ext/vector_float3.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace shared {
struct HeightTileSurfaceQuad;
} // namespace shared

namespace client {

class HeightTileDrawIndex final {
public:
    struct DirectionalSpan final {
        uint32_t instance_count = 0U;
        int32_t minimum_origin = 0;
        int32_t maximum_origin = 0;
    };

    using DirectionalSpans = std::array<DirectionalSpan, 6>;

    struct Entry final {
        shared::HeightTileCoordinate coordinate{};
        StoneIndirectDraws::Range range;
        glm::vec3 minimum{};
        glm::vec3 maximum{};
        DirectionalSpans directional_spans{};
    };

    struct Work final {
        uint32_t groups = 0U;
        uint32_t records = 0U;
        uint64_t group_plane_tests = 0U;
        uint64_t tile_plane_tests = 0U;
        uint32_t wrap_ambiguous_groups = 0U;
        uint64_t group_and_tile_traversal_duration_ns = 0U;
        uint64_t pending_range_order_duration_ns = 0U;
        uint64_t visible_range_merge_duration_ns = 0U;
    };

    HeightTileDrawIndex() = default;
    HeightTileDrawIndex(HeightTileDrawIndex const&) = delete;
    HeightTileDrawIndex& operator=(HeightTileDrawIndex const&) = delete;

    [[nodiscard]]
    static DirectionalSpans directionalSpans(std::span<shared::HeightTileSurfaceQuad const> quads);
    static void appendVisibleRanges(
        StoneIndirectDraws::Range range,
        DirectionalSpans const& spans,
        glm::dvec3 camera_position,
        std::vector<StoneIndirectDraws::Range>& visible_ranges
    );

    void upsert(Entry entry);
    void remove(shared::HeightTileCoordinate coordinate) noexcept;
    void removeBatch(std::span<shared::HeightTileCoordinate const> coordinates) noexcept;
    void clear() noexcept;

    [[nodiscard]]
    Work collect(
        VulkanFrustum const& frustum,
        glm::dvec3 camera_position,
        std::vector<StoneIndirectDraws::Range>& visible_ranges
    );

    [[nodiscard]]
    bool hasFallbackRecords() const noexcept { return m_fallback_record_count != 0U; }
    [[nodiscard]]
    uint32_t lastUpdateSlotsInspected() const noexcept { return m_last_update_slots_inspected; }

private:
    static constexpr int32_t GROUP_SIDE = 8;
    static constexpr uint32_t GROUP_CAPACITY = 64U;

    struct Group final {
        std::array<uint16_t, GROUP_CAPACITY> member_indices{};
        std::vector<Entry> entries;
        glm::vec3 minimum{};
        glm::vec3 maximum{};
        glm::vec2 minimum_center{};
        glm::vec2 maximum_center{};
        glm::vec3 shift{};
        VulkanFrustum::Classification classification;
        uint32_t uncertified_directional_bounds_entries = 0U;
        bool same_wrap_image = false;
    };

    using Groups = std::unordered_map<uint64_t, Group>;
    using RangeStarts = std::unordered_set<uint32_t>;

    struct RetiredStorage final {
        RangeStarts::node_type range_start;
        Groups::node_type group;
    };

    struct PendingRange final {
        uint32_t order_key;
        StoneIndirectDraws::Range range;
    };

    struct Address final {
        uint64_t key;
        uint32_t member;
    };

    [[nodiscard]]
    static Address address(shared::HeightTileCoordinate coordinate) noexcept;
    [[nodiscard]]
    static uint64_t rangeStartNodeStorageBytes() noexcept;
    [[nodiscard]]
    static uint64_t groupNodeStorageBytes(Group const& group) noexcept;
    static void addRetiredStorageBytes(uint64_t& total, uint64_t bytes) noexcept;
    void refresh(Group& group) noexcept;
    void orderPendingRanges();

    Groups m_groups;
    RangeStarts m_range_starts;
    std::vector<PendingRange> m_pending_ranges;
    std::vector<PendingRange> m_sort_workspace;
    std::vector<uint64_t> m_sort_offsets;
    std::optional<VulkanFrustum> m_cached_frustum;
    glm::dvec3 m_cached_camera_position{};
    std::vector<StoneIndirectDraws::Range> m_cached_visible_ranges;
    uint32_t m_fallback_record_count = 0U;
    uint32_t m_last_update_slots_inspected = 0U;
    bool m_visibility_cache_valid = false;
    HeightTileInstanceReaper<RetiredStorage> m_storage_reaper;
};

} // namespace client
