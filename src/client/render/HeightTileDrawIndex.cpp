#include <client/render/HeightTileDrawIndex.hpp>

#include <client/render/StoneIndirectDraws.hpp>
#include <client/render/VulkanFrustum.hpp>

#include <shared/world/HeightTileSurfaceMesher.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <glm/common.hpp>
#include <glm/ext/vector_double3.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace client {

namespace {

constexpr double WORLD_WRAP_PERIOD = 65'536.0;
constexpr double FACING_ROUNDING_TOLERANCE = 8.0 * std::numeric_limits<float>::epsilon();

[[nodiscard]]
bool backFacing(
    HeightTileDrawIndex::DirectionalSpan const span,
    uint32_t const direction,
    glm::dvec3 const camera_position
) noexcept {
    int32_t const axis = static_cast<int32_t>(direction / 2U);
    double const eye = camera_position[axis];
    double const origin = std::floor(eye);
    double minimum = static_cast<double>(span.minimum_origin) - origin;
    double maximum = static_cast<double>(span.maximum_origin) - origin;
    if (axis < 2) {
        auto const shift = [](double const relative) {
            return relative > WORLD_WRAP_PERIOD * 0.5 ? -WORLD_WRAP_PERIOD
                : relative < -WORLD_WRAP_PERIOD * 0.5 ? WORLD_WRAP_PERIOD : 0.0;
        };
        double const minimum_shift = shift(minimum);
        double const maximum_shift = shift(maximum);
        // The shader wraps each quad origin, so a bucket crossing its cut has no single facing envelope.
        if (minimum_shift != maximum_shift) {
            return false;
        }
        minimum += minimum_shift;
        maximum += maximum_shift;
    }
    double const positive_plane = static_cast<double>(direction & 1U);
    minimum += positive_plane;
    maximum += positive_plane;
    double const relative_eye = eye - origin;
    double const tolerance = FACING_ROUNDING_TOLERANCE
        * std::max({ std::abs(minimum), std::abs(maximum), 1.0 });
    return (direction & 1U) != 0U ? relative_eye < minimum - tolerance : relative_eye > maximum + tolerance;
}

void appendRange(
    StoneIndirectDraws::Range const range,
    std::vector<StoneIndirectDraws::Range>& visible_ranges
)
{
    if (!visible_ranges.empty()
        && visible_ranges.back().first_instance + visible_ranges.back().instance_count == range.first_instance) {
        visible_ranges.back().instance_count += range.instance_count;
    } else {
        visible_ranges.push_back(range);
    }
}

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

HeightTileDrawIndex::DirectionalSpans HeightTileDrawIndex::directionalSpans(
    std::span<shared::HeightTileSurfaceQuad const> const quads
)
{
    DirectionalSpans spans{};
    for (shared::HeightTileSurfaceQuad const& quad : quads) {
        uint32_t const direction = static_cast<uint32_t>(quad.direction);
        if (direction >= spans.size()) {
            throw std::invalid_argument("height tile direction span contains an invalid direction");
        }
        int32_t const plane_origin = direction <= 1U ? quad.x : direction <= 3U ? quad.y : quad.z;
        DirectionalSpan& span = spans[direction];
        if (span.instance_count == 0U) {
            span.minimum_origin = plane_origin;
            span.maximum_origin = plane_origin;
        } else {
            span.minimum_origin = std::min(span.minimum_origin, plane_origin);
            span.maximum_origin = std::max(span.maximum_origin, plane_origin);
        }
        ++span.instance_count;
    }
    return spans;
}

void HeightTileDrawIndex::appendVisibleRanges(
    StoneIndirectDraws::Range const range,
    DirectionalSpans const& spans,
    glm::dvec3 const camera_position,
    std::vector<StoneIndirectDraws::Range>& visible_ranges
)
{
    uint32_t first_instance = range.first_instance;
    for (uint32_t direction = 0U; direction < spans.size(); ++direction) {
        DirectionalSpan const span = spans[direction];
        if (span.instance_count != 0U && !backFacing(span, direction, camera_position)) {
            appendRange({ first_instance, span.instance_count }, visible_ranges);
        }
        first_instance += span.instance_count;
    }
    if (first_instance == range.first_instance) {
        appendRange(range, visible_ranges);
    }
}

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
    uint64_t directional_count = 0U;
    for (DirectionalSpan const span : entry.directional_spans) {
        directional_count += span.instance_count;
        if (span.instance_count != 0U && span.minimum_origin > span.maximum_origin) {
            throw std::invalid_argument("height tile draw index direction span is invalid");
        }
    }
    if (directional_count != 0U && directional_count != entry.range.instance_count) {
        throw std::invalid_argument("height tile draw index direction count does not match its range");
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
        appendVisibleRanges(entry.range, entry.directional_spans, camera_position, visible_ranges);
    }
    return work;
}

} // namespace client
