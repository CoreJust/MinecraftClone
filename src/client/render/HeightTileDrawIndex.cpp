#include <client/render/HeightTileDrawIndex.hpp>

#include <glm/common.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace client {

namespace {

constexpr double WORLD_WRAP_PERIOD = 65'536.0;

[[nodiscard]]
float wrapShift(double const camera, float const center) noexcept
{
    return static_cast<float>(std::round((camera - static_cast<double>(center)) / WORLD_WRAP_PERIOD)
        * WORLD_WRAP_PERIOD);
}

[[nodiscard]]
bool isFallback(shared::HeightTileCoordinate const coordinate) noexcept
{
    return coordinate.x < 0 || coordinate.y < 0;
}

} // namespace

HeightTileDrawIndex::Address HeightTileDrawIndex::address(shared::HeightTileCoordinate const coordinate) noexcept
{
    int32_t const group_x = coordinate.x / GROUP_SIDE - (coordinate.x % GROUP_SIDE < 0 ? 1 : 0);
    int32_t const group_y = coordinate.y / GROUP_SIDE - (coordinate.y % GROUP_SIDE < 0 ? 1 : 0);
    uint32_t const local_x = static_cast<uint32_t>(static_cast<int64_t>(coordinate.x)
        - static_cast<int64_t>(group_x) * GROUP_SIDE);
    uint32_t const local_y = static_cast<uint32_t>(static_cast<int64_t>(coordinate.y)
        - static_cast<int64_t>(group_y) * GROUP_SIDE);
    return {
        .key = (static_cast<uint64_t>(static_cast<uint32_t>(group_x)) << 32U) | static_cast<uint32_t>(group_y),
        .member = local_y * static_cast<uint32_t>(GROUP_SIDE) + local_x,
    };
}

void HeightTileDrawIndex::refresh(Group& group) noexcept
{
    m_last_update_slots_inspected = GROUP_CAPACITY;
    bool first = true;
    for (Record const* const record : group.records) {
        if (record == nullptr) {
            continue;
        }
        Entry const& entry = record->entry;
        glm::vec2 const center = glm::vec2((entry.minimum + entry.maximum) * 0.5F);
        if (first) {
            group.minimum = entry.minimum;
            group.maximum = entry.maximum;
            group.minimum_center = center;
            group.maximum_center = center;
            first = false;
        } else {
            group.minimum = glm::min(group.minimum, entry.minimum);
            group.maximum = glm::max(group.maximum, entry.maximum);
            group.minimum_center = glm::min(group.minimum_center, center);
            group.maximum_center = glm::max(group.maximum_center, center);
        }
    }
}

void HeightTileDrawIndex::upsert(Entry const entry)
{
    if (entry.range.instance_count == 0U
        || entry.range.first_instance > std::numeric_limits<uint32_t>::max() - entry.range.instance_count) {
        throw std::invalid_argument("height tile draw index range is invalid");
    }
    Address const location = address(entry.coordinate);
    auto [group_iterator, group_inserted] = m_groups.try_emplace(location.key);
    Group& group = group_iterator->second;
    Record const* const previous = group.records[location.member];
    if (previous != nullptr && previous->entry.range.first_instance == entry.range.first_instance) {
        m_records.at(entry.range.first_instance).entry = entry;
        refresh(group);
        return;
    }
    decltype(m_records)::iterator inserted;
    try {
        auto const result = m_records.emplace(entry.range.first_instance, Record{ .entry = entry, .group = &group });
        if (!result.second) {
            throw std::invalid_argument("height tile draw index range is already occupied");
        }
        inserted = result.first;
    } catch (...) {
        if (group_inserted) {
            m_groups.erase(group_iterator);
        }
        throw;
    }
    group.records[location.member] = &inserted->second;
    if (previous != nullptr) {
        m_records.erase(previous->entry.range.first_instance);
    } else {
        ++group.record_count;
        m_fallback_record_count += isFallback(entry.coordinate) ? 1U : 0U;
    }
    refresh(group);
}

void HeightTileDrawIndex::remove(shared::HeightTileCoordinate const coordinate) noexcept
{
    m_last_update_slots_inspected = 0U;
    Address const location = address(coordinate);
    auto const group_iterator = m_groups.find(location.key);
    if (group_iterator == m_groups.end()) {
        return;
    }
    Group& group = group_iterator->second;
    Record const* const record = group.records[location.member];
    if (record == nullptr) {
        return;
    }
    m_records.erase(record->entry.range.first_instance);
    group.records[location.member] = nullptr;
    m_fallback_record_count -= isFallback(coordinate) ? 1U : 0U;
    if (--group.record_count == 0U) {
        m_groups.erase(group_iterator);
    } else {
        refresh(group);
    }
}

void HeightTileDrawIndex::clear() noexcept
{
    m_records.clear();
    m_groups.clear();
    m_fallback_record_count = 0U;
    m_last_update_slots_inspected = 0U;
}

HeightTileDrawIndex::Work HeightTileDrawIndex::collect(
    VulkanFrustum const& frustum,
    glm::dvec3 const camera_position,
    std::vector<StoneIndirectDraws::Range>& visible_ranges
)
{
    Work work{ .groups = static_cast<uint32_t>(m_groups.size()), .records = static_cast<uint32_t>(m_records.size()) };
    visible_ranges.clear();
    visible_ranges.reserve(m_records.size());
    for (auto& [key, group] : m_groups) {
        static_cast<void>(key);
        group.shift = {
            wrapShift(camera_position.x, group.minimum_center.x),
            wrapShift(camera_position.y, group.minimum_center.y),
            0.0F,
        };
        group.same_wrap_image = group.shift.x == wrapShift(camera_position.x, group.maximum_center.x)
            && group.shift.y == wrapShift(camera_position.y, group.maximum_center.y);
        group.classification = {};
        if (group.same_wrap_image) {
            group.classification = frustum.classify(group.minimum + group.shift, group.maximum + group.shift);
            work.group_plane_tests += group.classification.plane_tests;
        } else {
            ++work.wrap_ambiguous_groups;
        }
    }
    for (auto const& [first_instance, record] : m_records) {
        static_cast<void>(first_instance);
        Entry const& entry = record.entry;
        Group const& group = *record.group;
        if (isFallback(entry.coordinate) || group.classification.outside) {
            continue;
        }
        if (group.classification.plane_mask != 0U) {
            WrappedBounds const bounds = group.same_wrap_image
                ? WrappedBounds{ .minimum = entry.minimum + group.shift, .maximum = entry.maximum + group.shift }
                : boundsNearestToCamera(entry.minimum, entry.maximum, camera_position);
            uint32_t plane_tests = 0U;
            bool const visible = frustum.intersects(
                bounds.minimum, bounds.maximum, group.classification.plane_mask, plane_tests
            );
            work.tile_plane_tests += plane_tests;
            if (!visible) {
                continue;
            }
        }
        StoneIndirectDraws::Range const range = entry.range;
        if (!visible_ranges.empty()
            && visible_ranges.back().first_instance + visible_ranges.back().instance_count == range.first_instance) {
            visible_ranges.back().instance_count += range.instance_count;
        } else {
            visible_ranges.push_back(range);
        }
    }
    return work;
}

} // namespace client
