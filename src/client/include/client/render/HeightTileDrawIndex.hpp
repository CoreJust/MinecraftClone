#pragma once

#include <client/render/StoneIndirectDraws.hpp>
#include <client/render/VulkanFrustum.hpp>

#include <shared/world/WorldGeneration.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace client {

class HeightTileDrawIndex final {
public:
    struct Entry final {
        shared::HeightTileCoordinate coordinate{};
        StoneIndirectDraws::Range range;
        glm::vec3 minimum{};
        glm::vec3 maximum{};
    };

    struct Work final {
        uint32_t groups = 0U;
        uint32_t records = 0U;
        uint64_t group_plane_tests = 0U;
        uint64_t tile_plane_tests = 0U;
        uint32_t wrap_ambiguous_groups = 0U;
    };

    HeightTileDrawIndex() = default;
    HeightTileDrawIndex(HeightTileDrawIndex const&) = delete;
    HeightTileDrawIndex& operator=(HeightTileDrawIndex const&) = delete;

    void upsert(Entry entry);
    void remove(shared::HeightTileCoordinate coordinate) noexcept;
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
    static constexpr int32_t GROUP_SIDE = 16;
    static constexpr uint32_t GROUP_CAPACITY = 256U;

    struct Group;

    struct Record final {
        Entry entry;
        Group* group;
    };

    struct Group final {
        std::array<Record const*, GROUP_CAPACITY> records{};
        uint32_t record_count = 0U;
        glm::vec3 minimum{};
        glm::vec3 maximum{};
        glm::vec2 minimum_center{};
        glm::vec2 maximum_center{};
        glm::vec3 shift{};
        VulkanFrustum::Classification classification;
        bool same_wrap_image = false;
    };

    struct Address final {
        uint64_t key;
        uint32_t member;
    };

    [[nodiscard]]
    static Address address(shared::HeightTileCoordinate coordinate) noexcept;
    void refresh(Group& group) noexcept;

    std::map<uint32_t, Record> m_records;
    std::unordered_map<uint64_t, Group> m_groups;
    uint32_t m_fallback_record_count = 0U;
    uint32_t m_last_update_slots_inspected = 0U;
};

} // namespace client
