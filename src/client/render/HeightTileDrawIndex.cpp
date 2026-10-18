#include <client/render/HeightTileDrawIndex.hpp>

#include <client/render/HeightTileInstanceReaper.hpp>
#include <client/render/StoneIndirectDraws.hpp>
#include <client/render/VulkanFrustum.hpp>

#include <shared/world/HeightTileSurfaceMesher.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <glm/common.hpp>
#include <glm/ext/vector_double3.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace client {

namespace {

constexpr double WORLD_WRAP_PERIOD = 65'536.0;
constexpr double FACING_ROUNDING_TOLERANCE = 8.0 * std::numeric_limits<float>::epsilon();
constexpr uint64_t HASH_NODE_OVERHEAD_BYTES = static_cast<uint64_t>(sizeof(void*)) * 4U;

enum class GroupFacingMode : uint8_t {
    PerSpan,
    AlwaysVisible,
    AlwaysBackfacing,
};

using GroupFacingModes = std::array<GroupFacingMode, 6>;

struct FacingCamera final {
    glm::dvec3 origin{};
    glm::dvec3 relative_eye{};
};

[[nodiscard]]
FacingCamera prepareFacingCamera(glm::dvec3 const camera_position) noexcept
{
    FacingCamera camera;
    for (int32_t axis = 0; axis < 3; ++axis) {
        double const eye = camera_position[axis];
        camera.origin[axis] = std::floor(eye);
        camera.relative_eye[axis] = eye - camera.origin[axis];
    }
    return camera;
}

[[nodiscard]]
double facingWrapShift(double const relative) noexcept
{
    return relative > WORLD_WRAP_PERIOD * 0.5 ? -WORLD_WRAP_PERIOD
        : relative < -WORLD_WRAP_PERIOD * 0.5 ? WORLD_WRAP_PERIOD : 0.0;
}

[[nodiscard]]
bool backFacing(
    HeightTileDrawIndex::DirectionalSpan const span,
    uint32_t const direction,
    FacingCamera const& camera
) noexcept {
    int32_t const axis = static_cast<int32_t>(direction / 2U);
    double const origin = camera.origin[axis];
    double minimum = static_cast<double>(span.minimum_origin) - origin;
    double maximum = static_cast<double>(span.maximum_origin) - origin;
    if (axis < 2) {
        double const minimum_shift = facingWrapShift(minimum);
        double const maximum_shift = facingWrapShift(maximum);
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
    double const relative_eye = camera.relative_eye[axis];
    double const tolerance = FACING_ROUNDING_TOLERANCE
        * std::max({ std::abs(minimum), std::abs(maximum), 1.0 });
    return (direction & 1U) != 0U ? relative_eye < minimum - tolerance : relative_eye > maximum + tolerance;
}

[[nodiscard]]
bool hasCertifiedDirectionalBounds(HeightTileDrawIndex::Entry const& entry) noexcept
{
    for (uint32_t direction = 0U; direction < entry.directional_spans.size(); ++direction) {
        HeightTileDrawIndex::DirectionalSpan const span = entry.directional_spans[direction];
        if (span.instance_count == 0U) {
            continue;
        }
        int32_t const axis = static_cast<int32_t>(direction / 2U);
        double const lower = static_cast<double>(entry.minimum[axis]);
        double const upper = static_cast<double>(entry.maximum[axis]);
        if (!std::isfinite(lower) || !std::isfinite(upper)
            || static_cast<double>(span.minimum_origin) < lower
            || static_cast<double>(span.maximum_origin) > upper) {
            return false;
        }
    }
    return true;
}

[[nodiscard]]
GroupFacingModes classifyGroupFacing(
    WrappedBounds const bounds,
    bool const all_directional_bounds_certified,
    FacingCamera const& camera
) noexcept
{
    GroupFacingModes modes{};
    if (!all_directional_bounds_certified) {
        return modes;
    }
    for (uint32_t direction = 0U; direction < modes.size(); ++direction) {
        int32_t const axis = static_cast<int32_t>(direction / 2U);
        double minimum = static_cast<double>(bounds.minimum[axis]) - camera.origin[axis];
        double maximum = static_cast<double>(bounds.maximum[axis]) - camera.origin[axis];
        if (axis < 2) {
            double const minimum_shift = facingWrapShift(minimum);
            double const maximum_shift = facingWrapShift(maximum);
            if (minimum_shift != maximum_shift) {
                continue;
            }
            minimum += minimum_shift;
            maximum += maximum_shift;
        }
        double const positive_plane = static_cast<double>(direction & 1U);
        minimum += positive_plane;
        maximum += positive_plane;
        double const relative_eye = camera.relative_eye[axis];
        double const tolerance = FACING_ROUNDING_TOLERANCE
            * std::max({ std::abs(minimum), std::abs(maximum), 1.0 });
        if ((direction & 1U) != 0U) {
            if (relative_eye < minimum - tolerance) {
                modes[direction] = GroupFacingMode::AlwaysBackfacing;
            } else if (relative_eye >= maximum) {
                modes[direction] = GroupFacingMode::AlwaysVisible;
            }
        } else if (relative_eye > maximum + tolerance) {
            modes[direction] = GroupFacingMode::AlwaysBackfacing;
        } else if (relative_eye <= minimum) {
            modes[direction] = GroupFacingMode::AlwaysVisible;
        }
    }
    return modes;
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

template<typename Append>
    requires std::invocable<Append&, StoneIndirectDraws::Range, uint32_t>
void forEachVisibleRange(
    StoneIndirectDraws::Range const range,
    HeightTileDrawIndex::DirectionalSpans const& spans,
    FacingCamera const& camera,
    GroupFacingModes const& group_facing_modes,
    Append&& append
)
{
    uint32_t first_instance = range.first_instance;
    for (uint32_t direction = 0U; direction < spans.size(); ++direction) {
        HeightTileDrawIndex::DirectionalSpan const span = spans[direction];
        if (span.instance_count != 0U) {
            GroupFacingMode const mode = group_facing_modes[direction];
            if (mode != GroupFacingMode::AlwaysBackfacing
                && (mode == GroupFacingMode::AlwaysVisible || !backFacing(span, direction, camera))) {
                append(StoneIndirectDraws::Range{ first_instance, span.instance_count }, direction);
            }
        }
        first_instance += span.instance_count;
    }
    if (first_instance == range.first_instance) {
        append(range, 0U);
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
    FacingCamera const camera = prepareFacingCamera(camera_position);
    GroupFacingModes const per_span_modes{};
    forEachVisibleRange(range, spans, camera, per_span_modes,
        [&](StoneIndirectDraws::Range const selected, uint32_t) {
        appendRange(selected, visible_ranges);
    });
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

uint64_t HeightTileDrawIndex::rangeStartNodeStorageBytes() noexcept
{
    return static_cast<uint64_t>(sizeof(RangeStarts::value_type)) + HASH_NODE_OVERHEAD_BYTES;
}

uint64_t HeightTileDrawIndex::groupNodeStorageBytes(Group const& group) noexcept
{
    uint64_t const fixed_bytes = static_cast<uint64_t>(sizeof(Groups::value_type)) + HASH_NODE_OVERHEAD_BYTES;
    uint64_t const entry_capacity = static_cast<uint64_t>(group.entries.capacity());
    uint64_t const entry_size = static_cast<uint64_t>(sizeof(Entry));
    uint64_t const maximum = std::numeric_limits<uint64_t>::max();
    if (entry_capacity > (maximum - fixed_bytes) / entry_size) {
        return maximum;
    }
    return fixed_bytes + entry_capacity * entry_size;
}

void HeightTileDrawIndex::addRetiredStorageBytes(uint64_t& total, uint64_t const bytes) noexcept
{
    uint64_t const maximum = std::numeric_limits<uint64_t>::max();
    total = bytes > maximum - total ? maximum : total + bytes;
}

void HeightTileDrawIndex::refresh(Group& group) noexcept
{
    m_last_update_slots_inspected = static_cast<uint32_t>(group.entries.size());
    bool first = true;
    for (Entry const& entry : group.entries) {
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
    uint16_t const member_index = group.member_indices[location.member];
    bool const replacing = member_index != 0U;
    if (replacing && group.entries[member_index - 1U].range.first_instance == entry.range.first_instance) {
        Entry& previous = group.entries[member_index - 1U];
        bool const previous_certified = hasCertifiedDirectionalBounds(previous);
        previous = entry;
        if (!previous_certified) {
            --group.uncertified_directional_bounds_entries;
        }
        if (!hasCertifiedDirectionalBounds(entry)) {
            ++group.uncertified_directional_bounds_entries;
        }
        refresh(group);
        m_visibility_cache_valid = false;
        return;
    }
    std::vector<RetiredStorage> retired_storage;
    uint64_t retired_owned_storage_bytes = 0U;
    uint32_t previous_range_start = 0U;
    if (replacing) {
        previous_range_start = group.entries[member_index - 1U].range.first_instance;
        retired_storage.reserve(1U);
    }
    bool range_inserted = false;
    try {
        auto const result = m_range_starts.insert(entry.range.first_instance);
        if (!result.second) {
            throw std::invalid_argument("height tile draw index range is already occupied");
        }
        range_inserted = true;
        if (!replacing) {
            group.entries.push_back(entry);
        }
    } catch (...) {
        if (range_inserted || group_inserted) {
            std::vector<RetiredStorage> rollback_storage;
            bool rollback_storage_reserved = false;
            try {
                rollback_storage.reserve(1U);
                rollback_storage_reserved = true;
            } catch (...) {
                rollback_storage_reserved = false;
            }
            if (!rollback_storage_reserved) {
                m_storage_reaper.recordSynchronousFallback();
                if (range_inserted) {
                    m_range_starts.erase(entry.range.first_instance);
                }
                if (group_inserted) {
                    m_groups.erase(group_iterator);
                }
            } else {
                RetiredStorage retired;
                if (range_inserted) {
                    auto const range_start = m_range_starts.find(entry.range.first_instance);
                    if (range_start != m_range_starts.end()) {
                        retired.range_start = m_range_starts.extract(range_start);
                        addRetiredStorageBytes(retired_owned_storage_bytes, rangeStartNodeStorageBytes());
                    }
                }
                if (group_inserted) {
                    addRetiredStorageBytes(
                        retired_owned_storage_bytes, groupNodeStorageBytes(group_iterator->second));
                    retired.group = m_groups.extract(group_iterator);
                }
                if (retired.range_start || retired.group) {
                    rollback_storage.push_back(std::move(retired));
                    static_cast<void>(m_storage_reaper.retire(
                        rollback_storage, retired_owned_storage_bytes));
                }
            }
        }
        throw;
    }
    if (replacing) {
        Entry& previous = group.entries[member_index - 1U];
        bool const previous_certified = hasCertifiedDirectionalBounds(previous);
        auto const previous_range = m_range_starts.find(previous_range_start);
        if (previous_range != m_range_starts.end()) {
            RetiredStorage retired;
            retired.range_start = m_range_starts.extract(previous_range);
            addRetiredStorageBytes(retired_owned_storage_bytes, rangeStartNodeStorageBytes());
            retired_storage.push_back(std::move(retired));
        }
        previous = entry;
        if (!previous_certified) {
            --group.uncertified_directional_bounds_entries;
        }
        if (!hasCertifiedDirectionalBounds(entry)) {
            ++group.uncertified_directional_bounds_entries;
        }
    } else {
        group.member_indices[location.member] = static_cast<uint16_t>(group.entries.size());
        m_fallback_record_count += isFallback(entry.coordinate) ? 1U : 0U;
        if (!hasCertifiedDirectionalBounds(entry)) {
            ++group.uncertified_directional_bounds_entries;
        }
    }
    refresh(group);
    m_visibility_cache_valid = false;
    if (!retired_storage.empty()) {
        static_cast<void>(m_storage_reaper.retire(retired_storage, retired_owned_storage_bytes));
    }
}

void HeightTileDrawIndex::remove(shared::HeightTileCoordinate const coordinate) noexcept
{
    std::array<shared::HeightTileCoordinate, 1U> const coordinates{ coordinate };
    removeBatch(coordinates);
}

void HeightTileDrawIndex::removeBatch(
    std::span<shared::HeightTileCoordinate const> const coordinates
) noexcept
{
    static constexpr size_t GROUP_KEYS_PER_BATCH = 128U;
    std::array<uint64_t, GROUP_KEYS_PER_BATCH> affected_group_keys{};
    m_last_update_slots_inspected = 0U;
    std::vector<RetiredStorage> retired_storage;
    uint64_t retired_storage_count = 0U;
    uint64_t retired_owned_storage_bytes = 0U;
    auto const remove_entries = [&](bool const retire_storage) noexcept {
        for (size_t batch_start = 0U; batch_start < coordinates.size(); batch_start += GROUP_KEYS_PER_BATCH) {
            size_t const batch_size = std::min(GROUP_KEYS_PER_BATCH, coordinates.size() - batch_start);
            size_t affected_group_count = 0U;
            for (shared::HeightTileCoordinate const coordinate : coordinates.subspan(batch_start, batch_size)) {
                Address const location = address(coordinate);
                auto const group_iterator = m_groups.find(location.key);
                if (group_iterator == m_groups.end()) {
                    continue;
                }
                Group& group = group_iterator->second;
                uint16_t const member_index = group.member_indices[location.member];
                if (member_index == 0U) {
                    continue;
                }
                m_visibility_cache_valid = false;
                Entry& entry = group.entries[member_index - 1U];
                RetiredStorage retired;
                auto const range_start = m_range_starts.find(entry.range.first_instance);
                if (range_start != m_range_starts.end()) {
                    if (retire_storage) {
                        retired.range_start = m_range_starts.extract(range_start);
                        addRetiredStorageBytes(
                            retired_owned_storage_bytes, rangeStartNodeStorageBytes());
                    } else {
                        m_range_starts.erase(range_start);
                    }
                }
                if (!hasCertifiedDirectionalBounds(entry)) {
                    --group.uncertified_directional_bounds_entries;
                }
                entry = group.entries.back();
                group.member_indices[address(entry.coordinate).member] = member_index;
                group.entries.pop_back();
                group.member_indices[location.member] = 0U;
                m_fallback_record_count -= isFallback(coordinate) ? 1U : 0U;
                if (retire_storage && group.entries.empty()) {
                    addRetiredStorageBytes(
                        retired_owned_storage_bytes, groupNodeStorageBytes(group));
                    retired.group = m_groups.extract(group_iterator);
                }
                if (retire_storage && (retired.range_start || retired.group)) {
                    RetiredStorage& slot = retired_storage[retired_storage_count++];
                    slot.range_start.swap(retired.range_start);
                    slot.group.swap(retired.group);
                }
                affected_group_keys[affected_group_count++] = location.key;
            }
            std::span<uint64_t> const affected_groups{ affected_group_keys.data(), affected_group_count };
            std::ranges::sort(affected_groups);
            auto const unique_end = std::ranges::unique(affected_groups);
            for (auto group_key = affected_groups.begin(); group_key != unique_end.begin(); ++group_key) {
                auto const group_iterator = m_groups.find(*group_key);
                if (group_iterator == m_groups.end()) {
                    continue;
                }
                if (group_iterator->second.entries.empty()) {
                    m_groups.erase(group_iterator);
                } else {
                    refresh(group_iterator->second);
                }
            }
        }
    };
    static_assert(noexcept(std::declval<RangeStarts::node_type&>().swap(std::declval<RangeStarts::node_type&>())));
    static_assert(noexcept(std::declval<Groups::node_type&>().swap(std::declval<Groups::node_type&>())));
    try {
        retired_storage.resize(std::min(coordinates.size(), m_range_starts.size()));
    } catch (...) {
        m_storage_reaper.recordSynchronousFallback();
        remove_entries(false);
        return;
    }
    remove_entries(true);
    static_cast<void>(m_storage_reaper.retire(retired_storage, retired_owned_storage_bytes));
}

void HeightTileDrawIndex::clear() noexcept
{
    m_storage_reaper.drain();
    m_range_starts.clear();
    m_groups.clear();
    m_pending_ranges.clear();
    m_sort_workspace.clear();
    m_cached_frustum.reset();
    m_cached_visible_ranges.clear();
    m_fallback_record_count = 0U;
    m_last_update_slots_inspected = 0U;
    m_visibility_cache_valid = false;
}

void HeightTileDrawIndex::orderPendingRanges()
{
    static constexpr uint32_t DIGIT_BITS = 11U;
    static constexpr uint32_t DIGIT_COUNT = 1U << DIGIT_BITS;
    static constexpr uint32_t DIGIT_MASK = DIGIT_COUNT - 1U;
    static constexpr uint32_t PASS_COUNT = 3U;
    if (m_pending_ranges.size() < 2U) {
        return;
    }
    std::array<std::array<uint64_t, DIGIT_COUNT>, PASS_COUNT> offsets{};
    for (PendingRange const pending : m_pending_ranges) {
        for (uint32_t pass = 0U; pass < PASS_COUNT; ++pass) {
            uint32_t const digit = (pending.order_key >> (pass * DIGIT_BITS)) & DIGIT_MASK;
            ++offsets[pass][digit];
        }
    }
    m_sort_workspace.resize(m_pending_ranges.size());
    for (uint32_t pass = 0U; pass < PASS_COUNT; ++pass) {
        uint64_t first_index = 0U;
        for (uint64_t& offset : offsets[pass]) {
            uint64_t const count = offset;
            offset = first_index;
            first_index += count;
        }
        for (PendingRange const pending : m_pending_ranges) {
            uint32_t const digit = (pending.order_key >> (pass * DIGIT_BITS)) & DIGIT_MASK;
            m_sort_workspace[offsets[pass][digit]++] = pending;
        }
        m_pending_ranges.swap(m_sort_workspace);
    }
}

HeightTileDrawIndex::Work HeightTileDrawIndex::collect(
    VulkanFrustum const& frustum,
    glm::dvec3 const camera_position,
    std::vector<StoneIndirectDraws::Range>& visible_ranges
)
{
    Work work{ .groups = static_cast<uint32_t>(m_groups.size()),
        .records = static_cast<uint32_t>(m_range_starts.size()) };
    if (m_visibility_cache_valid && m_cached_frustum.has_value()
        && m_cached_camera_position.x == camera_position.x
        && m_cached_camera_position.y == camera_position.y
        && m_cached_camera_position.z == camera_position.z
        && m_cached_frustum->hasSamePlanes(frustum)) {
        visible_ranges = m_cached_visible_ranges;
        return work;
    }
    m_visibility_cache_valid = false;
    FacingCamera const facing_camera = prepareFacingCamera(camera_position);
    visible_ranges.clear();
    visible_ranges.reserve(m_range_starts.size());
    m_pending_ranges.clear();
    m_pending_ranges.reserve(m_range_starts.size());
    std::chrono::steady_clock::time_point stage_started_at = std::chrono::steady_clock::now();
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
        if (group.classification.outside) {
            continue;
        }
        WrappedBounds const group_bounds{ .minimum = group.minimum, .maximum = group.maximum };
        GroupFacingModes const group_facing_modes = classifyGroupFacing(
            group_bounds, group.uncertified_directional_bounds_entries == 0U, facing_camera
        );
        for (Entry const& entry : group.entries) {
            if (isFallback(entry.coordinate)) {
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
            forEachVisibleRange(entry.range, entry.directional_spans, facing_camera, group_facing_modes,
                [&](StoneIndirectDraws::Range const selected, uint32_t) {
                    m_pending_ranges.push_back({
                        .order_key = entry.range.first_instance,
                        .range = selected,
                    });
                });
        }
    }
    std::chrono::steady_clock::time_point const traversal_finished_at = std::chrono::steady_clock::now();
    work.group_and_tile_traversal_duration_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(traversal_finished_at - stage_started_at).count()
    );
    // Owning tile order also preserves the established behavior of accepted overlapping ranges.
    orderPendingRanges();
    std::chrono::steady_clock::time_point const ordering_finished_at = std::chrono::steady_clock::now();
    work.pending_range_order_duration_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(ordering_finished_at - traversal_finished_at).count()
    );
    for (PendingRange const pending : m_pending_ranges) {
        appendRange(pending.range, visible_ranges);
    }
    std::chrono::steady_clock::time_point const merge_finished_at = std::chrono::steady_clock::now();
    work.visible_range_merge_duration_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(merge_finished_at - ordering_finished_at).count()
    );
    m_cached_frustum = frustum;
    m_cached_camera_position = camera_position;
    m_cached_visible_ranges = visible_ranges;
    m_visibility_cache_valid = true;
    return work;
}

} // namespace client
