#include <client/PlayerPreviewLod.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <core/common/Assert.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <map>
#include <utility>

namespace client {

namespace {

constexpr int32_t TILE_EXTENT = static_cast<int32_t>(
    shared::WorldExtent::WIDTH / shared::HEIGHT_TILE_SIDE_LENGTH
);

[[nodiscard]] bool contains(
    shared::HeightTileSurfaceBounds const outer,
    shared::HeightTileSurfaceBounds const inner
) noexcept
{
    return outer.min_x_blocks <= inner.min_x_blocks && outer.max_x_blocks >= inner.max_x_blocks
        && outer.min_y_blocks <= inner.min_y_blocks && outer.max_y_blocks >= inner.max_y_blocks
        && outer.min_z_blocks <= inner.min_z_blocks && outer.max_z_blocks >= inner.max_z_blocks;
}

[[nodiscard]] int64_t shortestTileDisplacement(int32_t const from, int32_t const to) noexcept
{
    int64_t displacement = static_cast<int64_t>(to) - from;
    if (displacement > TILE_EXTENT / 2) {
        displacement -= TILE_EXTENT;
    } else if (displacement < -TILE_EXTENT / 2) {
        displacement += TILE_EXTENT;
    }
    return displacement;
}

struct AxisOffsetExtrema final {
    int64_t minimum_offset = 0;
    int64_t maximum_offset = 0;
    int32_t minimum_coordinate = 0;
    int32_t maximum_coordinate = 0;
    bool present = false;
};

[[nodiscard]] AxisOffsetExtrema wrappedAxisExtrema(
    std::map<int32_t, uint32_t> const& coordinates,
    int32_t const center
) noexcept
{
    int32_t const half_extent = TILE_EXTENT / 2;
    std::array<std::pair<int32_t, int32_t>, 3U> const intervals{{
        { 0, center - half_extent - 1 },
        { std::max(0, center - half_extent), std::min(TILE_EXTENT - 1, center + half_extent) },
        { center + half_extent + 1, TILE_EXTENT - 1 },
    }};
    AxisOffsetExtrema extrema;
    auto const consider = [&extrema, center](int32_t const coordinate) {
        int64_t const offset = shortestTileDisplacement(center, coordinate);
        if (!extrema.present || offset < extrema.minimum_offset) {
            extrema.minimum_offset = offset;
            extrema.minimum_coordinate = coordinate;
        }
        if (!extrema.present || offset > extrema.maximum_offset) {
            extrema.maximum_offset = offset;
            extrema.maximum_coordinate = coordinate;
        }
        extrema.present = true;
    };
    for (auto const [first_coordinate, last_coordinate] : intervals) {
        if (first_coordinate > last_coordinate) {
            continue;
        }
        auto const first = coordinates.lower_bound(first_coordinate);
        auto const after_last = coordinates.upper_bound(last_coordinate);
        if (first == after_last) {
            continue;
        }
        consider(first->first);
        consider(std::prev(after_last)->first);
    }
    return extrema;
}

[[nodiscard]] bool axisImageTranslationMatches(
    shared::HeightTileKey const old_center,
    shared::HeightTileKey const new_center,
    int32_t const coordinate,
    bool const x_axis,
    double const expected_shift
) noexcept
{
    shared::HeightTileKey const tile = x_axis
        ? shared::HeightTileKey{ .x = coordinate, .y = 0 }
        : shared::HeightTileKey{ .x = 0, .y = coordinate };
    shared::HeightTileSurfaceBounds const old_bounds = shared::HeightTileSurfaceMesher::boundsForTile(
        { old_center.x, old_center.y }, { tile.x, tile.y }, 0.0, 0.0
    );
    shared::HeightTileSurfaceBounds const new_bounds = shared::HeightTileSurfaceMesher::boundsForTile(
        { new_center.x, new_center.y }, { tile.x, tile.y }, 0.0, 0.0
    );
    return x_axis
        ? new_bounds.min_x_blocks - old_bounds.min_x_blocks == expected_shift
        : new_bounds.min_y_blocks - old_bounds.min_y_blocks == expected_shift;
}

[[nodiscard]] bool axisTranslationStable(
    std::map<int32_t, uint32_t> const& coordinates,
    shared::HeightTileKey const old_center,
    shared::HeightTileKey const new_center,
    bool const x_axis
) noexcept
{
    int32_t const old_coordinate = x_axis ? old_center.x : old_center.y;
    int32_t const new_coordinate = x_axis ? new_center.x : new_center.y;
    AxisOffsetExtrema const extrema = wrappedAxisExtrema(coordinates, old_coordinate);
    if (!extrema.present) {
        return true;
    }
    int64_t const center_shift = shortestTileDisplacement(old_coordinate, new_coordinate);
    int64_t const half_extent = TILE_EXTENT / 2;
    if (center_shift < extrema.maximum_offset - half_extent
        || center_shift > extrema.minimum_offset + half_extent) {
        return false;
    }
    constexpr double TILE_SIDE = static_cast<double>(shared::HEIGHT_TILE_SIDE_LENGTH);
    double const expected_shift = -static_cast<double>(center_shift) * TILE_SIDE;
    if (center_shift == extrema.maximum_offset - half_extent
        && !axisImageTranslationMatches(
            old_center,
            new_center,
            extrema.maximum_coordinate,
            x_axis,
            expected_shift)) {
        return false;
    }
    if (center_shift == extrema.minimum_offset + half_extent
        && !axisImageTranslationMatches(
            old_center,
            new_center,
            extrema.minimum_coordinate,
            x_axis,
            expected_shift)) {
        return false;
    }
    return true;
}

struct TileOffset final {
    int32_t x;
    int32_t y;
};

struct ImageTranslation final {
    double x;
    double y;
};

[[nodiscard]] bool tileImageTranslationMatches(
    shared::HeightTileKey const old_center,
    shared::HeightTileKey const new_center,
    TileOffset const tile,
    ImageTranslation const expected_shift
) noexcept
{
    shared::HeightTileSurfaceBounds const old_bounds = shared::HeightTileSurfaceMesher::boundsForTile(
        { old_center.x, old_center.y }, { tile.x, tile.y }, 0.0, 0.0
    );
    shared::HeightTileSurfaceBounds const new_bounds = shared::HeightTileSurfaceMesher::boundsForTile(
        { new_center.x, new_center.y }, { tile.x, tile.y }, 0.0, 0.0
    );
    return new_bounds.min_x_blocks - old_bounds.min_x_blocks == expected_shift.x
        && new_bounds.min_y_blocks - old_bounds.min_y_blocks == expected_shift.y;
}

struct AxisDistanceBounds final {
    double viewer_min;
    double viewer_max;
    double surface_min;
    double surface_max;
};

[[nodiscard]] double farthestPoint(AxisDistanceBounds const bounds) noexcept
{
    double const below = std::max(0.0, bounds.viewer_min - bounds.surface_min);
    double const above = std::max(0.0, bounds.surface_max - bounds.viewer_max);
    return below > above ? bounds.surface_min : bounds.surface_max;
}

[[nodiscard]] shared::HeightTileSurfaceBounds farthestViewer(
    shared::HeightTileSurfaceBounds const viewer,
    shared::HeightTileSurfaceBounds const surface
) noexcept
{
    auto const endpoint = [](double const viewer_min, double const viewer_max,
        double const surface_min, double const surface_max) noexcept {
        return std::abs(surface_max - viewer_min) >= std::abs(surface_min - viewer_max)
            ? viewer_min
            : viewer_max;
    };
    double const x = endpoint(viewer.min_x_blocks, viewer.max_x_blocks, surface.min_x_blocks, surface.max_x_blocks);
    double const y = endpoint(viewer.min_y_blocks, viewer.max_y_blocks, surface.min_y_blocks, surface.max_y_blocks);
    double const z = endpoint(viewer.min_z_blocks, viewer.max_z_blocks, surface.min_z_blocks, surface.max_z_blocks);
    return { x, x, y, y, z, z };
}

[[nodiscard]] shared::HeightTileSurfaceBounds farthestSurface(
    shared::HeightTileSurfaceBounds const viewer,
    shared::HeightTileSurfaceBounds const surface
) noexcept
{
    double const x = farthestPoint({
        .viewer_min = viewer.min_x_blocks,
        .viewer_max = viewer.max_x_blocks,
        .surface_min = surface.min_x_blocks,
        .surface_max = surface.max_x_blocks,
    });
    double const y = farthestPoint({
        .viewer_min = viewer.min_y_blocks,
        .viewer_max = viewer.max_y_blocks,
        .surface_min = surface.min_y_blocks,
        .surface_max = surface.max_y_blocks,
    });
    double const z = farthestPoint({
        .viewer_min = viewer.min_z_blocks,
        .viewer_max = viewer.max_z_blocks,
        .surface_min = surface.min_z_blocks,
        .surface_max = surface.max_z_blocks,
    });
    return {
        .min_x_blocks = x,
        .max_x_blocks = x,
        .min_y_blocks = y,
        .max_y_blocks = y,
        .min_z_blocks = z,
        .max_z_blocks = z,
    };
}

[[nodiscard]] shared::HeightTileSurfaceBounds boundsInFrame(
    shared::HeightTileSurfaceBounds bounds,
    shared::HeightTileKey const center
) noexcept
{
    shared::HeightTileSurfaceBounds const origin = shared::HeightTileSurfaceMesher::boundsForTile(
        { center.x, center.y },
        {
            static_cast<int32_t>(bounds.min_x_blocks / shared::HEIGHT_TILE_SIDE_LENGTH),
            static_cast<int32_t>(bounds.min_y_blocks / shared::HEIGHT_TILE_SIDE_LENGTH),
        },
        0.0,
        0.0
    );
    double const x_shift = origin.min_x_blocks - bounds.min_x_blocks;
    double const y_shift = origin.min_y_blocks - bounds.min_y_blocks;
    bounds.min_x_blocks += x_shift;
    bounds.max_x_blocks += x_shift;
    bounds.min_y_blocks += y_shift;
    bounds.max_y_blocks += y_shift;
    return bounds;
}

} // namespace

PlayerPreviewInterestDelta playerPreviewInterestDelta(
    shared::HeightTileKey const previous_center,
    shared::HeightTileKey const next_center,
    uint32_t const radius
)
{
    return shared::heightTileInterestDelta(previous_center, next_center, radius);
}

bool playerPreviewTileWithinInterest(
    shared::HeightTileKey const center,
    shared::HeightTileKey const key,
    uint32_t const radius
)
{
    return shared::heightTileWithinInterest(center, key, radius);
}

std::vector<shared::HeightTileKey> playerPreviewLodRemeshClosure(
    std::span<shared::HeightTileKey const> changed_keys
)
{
    std::vector<shared::HeightTileKey> keys;
    if (changed_keys.size() <= keys.max_size() / 5U) {
        keys.reserve(changed_keys.size() * 5U);
    }
    playerPreviewLodRemeshClosure(changed_keys, keys);
    return keys;
}

void playerPreviewLodRemeshClosure(
    std::span<shared::HeightTileKey const> changed_keys,
    std::vector<shared::HeightTileKey>& output
)
{
    output.clear();
    for (shared::HeightTileKey const key : changed_keys) {
        shared::HeightTileKey const normalized = shared::normalizeHeightTileKey(key);
        output.push_back(normalized);
        output.push_back(shared::normalizeHeightTileKey({ .x = normalized.x - 1, .y = normalized.y }));
        output.push_back(shared::normalizeHeightTileKey({ .x = normalized.x + 1, .y = normalized.y }));
        output.push_back(shared::normalizeHeightTileKey({ .x = normalized.x, .y = normalized.y - 1 }));
        output.push_back(shared::normalizeHeightTileKey({ .x = normalized.x, .y = normalized.y + 1 }));
    }
    std::ranges::sort(output, [](shared::HeightTileKey const first, shared::HeightTileKey const second) {
        return first.y < second.y || (first.y == second.y && first.x < second.x);
    });
    auto const unique_end = std::ranges::unique(output);
    output.erase(unique_end.begin(), unique_end.end());
}

void PlayerPreviewLod::clear() noexcept
{
    m_buckets.clear();
    m_candidate_buckets.clear();
    m_dirty_buckets.clear();
    m_mandatory_buckets.clear();
    m_fine_buckets.clear();
    m_resident_x_coordinates.clear();
    m_resident_y_coordinates.clear();
    m_optional_cursor_id = 0U;
    m_optional_cursor_initialized = false;
    m_optional_fine_sweep_active = false;
    m_initialized = false;
}

uint32_t PlayerPreviewLod::bucketId(shared::HeightTileKey const key) noexcept
{
    constexpr uint32_t BUCKET_EXTENT = static_cast<uint32_t>(TILE_EXTENT) / BUCKET_SIDE;
    return static_cast<uint32_t>(key.y) / BUCKET_SIDE * BUCKET_EXTENT
        + static_cast<uint32_t>(key.x) / BUCKET_SIDE;
}

uint32_t PlayerPreviewLod::tileIndex(shared::HeightTileKey const key) noexcept
{
    return static_cast<uint32_t>(key.y) % BUCKET_SIDE * BUCKET_SIDE
        + static_cast<uint32_t>(key.x) % BUCKET_SIDE;
}

void PlayerPreviewLod::erase(shared::HeightTileKey const key)
{
    uint32_t const id = bucketId(key);
    auto const bucket = m_buckets.find(id);
    if (bucket == m_buckets.end()) {
        return;
    }
    std::optional<Tile>& tile = bucket->second.tiles[tileIndex(key)];
    if (!tile) {
        return;
    }
    if (tile->detail != shared::HeightTileSurfaceDetail::Fine) {
        --bucket->second.non_fine_tile_count;
    }
    --bucket->second.resident_tile_count;
    tile.reset();
    bucket->second.dirty = true;
    bucket->second.candidate_details = 0U;
    bucket->second.certified = false;
    m_dirty_buckets.insert(id);
    removeCertifiedOffset(key);
    updateBucketIndexes(id, bucket->second);
    if (bucket->second.resident_tile_count == 0U) {
        m_candidate_buckets.erase(id);
        m_mandatory_buckets.erase(id);
        m_fine_buckets.erase(id);
        m_dirty_buckets.erase(id);
        m_buckets.erase(bucket);
    }
}

void PlayerPreviewLod::eraseBatch(std::span<shared::HeightTileKey const> const keys)
{
    std::vector<uint32_t> affected_buckets;
    affected_buckets.reserve(keys.size());
    for (shared::HeightTileKey const key : keys) {
        uint32_t const id = bucketId(key);
        auto const bucket = m_buckets.find(id);
        if (bucket == m_buckets.end()) {
            continue;
        }
        std::optional<Tile>& tile = bucket->second.tiles[tileIndex(key)];
        if (!tile.has_value()) {
            continue;
        }
        if (tile->detail != shared::HeightTileSurfaceDetail::Fine) {
            --bucket->second.non_fine_tile_count;
        }
        --bucket->second.resident_tile_count;
        tile.reset();
        bucket->second.dirty = true;
        bucket->second.candidate_details = 0U;
        bucket->second.certified = false;
        m_dirty_buckets.insert(id);
        removeCertifiedOffset(key);
        updateBucketIndexes(id, bucket->second);
        affected_buckets.push_back(id);
    }
    std::ranges::sort(affected_buckets);
    auto const unique_end = std::ranges::unique(affected_buckets);
    affected_buckets.erase(unique_end.begin(), unique_end.end());
    for (uint32_t const id : affected_buckets) {
        auto const bucket = m_buckets.find(id);
        if (bucket != m_buckets.end() && bucket->second.resident_tile_count == 0U) {
            m_candidate_buckets.erase(id);
            m_mandatory_buckets.erase(id);
            m_fine_buckets.erase(id);
            m_dirty_buckets.erase(id);
            m_buckets.erase(bucket);
        }
    }
}

std::optional<shared::HeightTileSurfaceDetail> PlayerPreviewLod::detail(
    shared::HeightTileKey const key
) const noexcept
{
    auto const bucket = m_buckets.find(bucketId(key));
    if (bucket == m_buckets.end()) {
        return std::nullopt;
    }
    auto const& tile = bucket->second.tiles[tileIndex(key)];
    return tile ? std::optional{ tile->detail } : std::nullopt;
}

shared::HeightTileSurfaceBounds PlayerPreviewLod::surfaceBounds(Tile const& tile) const noexcept
{
    return shared::HeightTileSurfaceMesher::boundsForTile(
        { .x = m_center.x, .y = m_center.y },
        { .x = tile.key.x, .y = tile.key.y },
        tile.minimum,
        tile.maximum
    );
}

shared::HeightTileSurfaceDetail PlayerPreviewLod::selectDetail(
    shared::HeightTileKey const key,
    uint16_t const minimum,
    uint16_t const maximum
)
{
    shared::HeightTileSurfaceFaceAreas face_areas;
    for (uint32_t level = 0U; level < face_areas.maximum.size(); ++level) {
        double const cell_size = shared::HeightTileSurfaceFaceAreas::CELL_SIZES[level];
        face_areas.maximum[level] = std::max(cell_size * cell_size,
            cell_size * static_cast<double>(maximum - minimum));
    }
    return selectDetail(key, minimum, maximum, face_areas);
}

shared::HeightTileSurfaceDetail PlayerPreviewLod::selectDetail(
    shared::HeightTileKey const key,
    uint16_t const minimum,
    uint16_t const maximum,
    shared::HeightTileSurfaceFaceAreas const& face_areas
)
{
    ASSERT(key.x >= 0 && key.x < TILE_EXTENT && key.y >= 0 && key.y < TILE_EXTENT,
        "LOD tile keys must be normalized");
    ASSERT(minimum <= maximum, "LOD elevation bounds must be ordered");
    Bucket& bucket = m_buckets[bucketId(key)];
    std::optional<Tile>& previous = bucket.tiles[tileIndex(key)];
    Tile next{
        .key = key,
        .minimum = minimum,
        .maximum = maximum,
        .face_areas = face_areas,
        .detail = shared::HeightTileSurfaceDetail::Fine,
    };
    std::optional<shared::HeightTileSurfaceDetail> const current = previous
        ? std::optional{previous->detail}
        : std::nullopt;
    next.detail = m_initialized
        ? m_policy.detailFor(m_viewer_bounds, surfaceBounds(next), face_areas, current)
        : shared::HeightTileSurfaceDetail::Fine;
    if (!previous || previous->minimum != minimum || previous->maximum != maximum
        || previous->face_areas != face_areas || previous->detail != next.detail) {
        bucket.dirty = true;
        bucket.candidate_details = 0U;
        bucket.certified = false;
        m_dirty_buckets.insert(bucketId(key));
    }
    bool const had_fine_tiles = bucket.resident_tile_count > bucket.non_fine_tile_count;
    if (!previous) {
        ++bucket.resident_tile_count;
        if (next.detail != shared::HeightTileSurfaceDetail::Fine) {
            ++bucket.non_fine_tile_count;
        }
        addCertifiedOffset(key);
    } else if (previous->detail != next.detail) {
        if (previous->detail == shared::HeightTileSurfaceDetail::Fine) {
            ++bucket.non_fine_tile_count;
        } else if (next.detail == shared::HeightTileSurfaceDetail::Fine) {
            --bucket.non_fine_tile_count;
        }
    }
    previous = next;
    updateBucketIndexes(bucketId(key), bucket);
    if (!had_fine_tiles && bucket.resident_tile_count > bucket.non_fine_tile_count
        && m_optional_fine_sweep_active) {
        requestOptionalFineSweep();
    }
    return next.detail;
}

bool PlayerPreviewLod::imageTranslationStable(
    Bucket const& bucket,
    shared::HeightTileKey const current_center,
    double const center_offset_x,
    double const center_offset_y
) noexcept
{
    if (!bucket.certified) {
        return false;
    }
    constexpr double TILE_SIDE = static_cast<double>(shared::HEIGHT_TILE_SIDE_LENGTH);
    for (Group const& group : bucket.groups) {
        if (!group.present) {
            continue;
        }
        int32_t const min_x = static_cast<int32_t>(group.bounds.min_x_blocks / TILE_SIDE);
        int32_t const max_x = static_cast<int32_t>(group.bounds.max_x_blocks / TILE_SIDE) - 1;
        int32_t const min_y = static_cast<int32_t>(group.bounds.min_y_blocks / TILE_SIDE);
        int32_t const max_y = static_cast<int32_t>(group.bounds.max_y_blocks / TILE_SIDE) - 1;
        if (!tileImageTranslationMatches(
                bucket.certified_center,
                current_center,
                { .x = min_x, .y = min_y },
                { .x = -center_offset_x, .y = -center_offset_y }
            )
            || !tileImageTranslationMatches(
                bucket.certified_center,
                current_center,
                { .x = max_x, .y = max_y },
                { .x = -center_offset_x, .y = -center_offset_y }
            )) {
            return false;
        }
    }
    return true;
}

bool PlayerPreviewLod::centerTranslationStable(shared::HeightTileKey const current_center) const noexcept
{
    return axisTranslationStable(
            m_resident_x_coordinates, m_certified_center, current_center, true
        )
        && axisTranslationStable(
            m_resident_y_coordinates, m_certified_center, current_center, false
        );
}

void PlayerPreviewLod::addCertifiedOffset(shared::HeightTileKey const key)
{
    ++m_resident_x_coordinates[key.x];
    ++m_resident_y_coordinates[key.y];
}

void PlayerPreviewLod::removeCertifiedOffset(shared::HeightTileKey const key)
{
    auto const remove = [](std::map<int32_t, uint32_t>& coordinates, int32_t const value) {
        auto const coordinate = coordinates.find(value);
        if (coordinate == coordinates.end()) {
            return;
        }
        if (--coordinate->second == 0U) {
            coordinates.erase(coordinate);
        }
    };
    remove(m_resident_x_coordinates, key.x);
    remove(m_resident_y_coordinates, key.y);
}

void PlayerPreviewLod::updateBucketIndexes(uint32_t const id, Bucket& bucket)
{
    if (bucket.dirty || bucket.non_fine_tile_count != 0U) {
        m_mandatory_buckets.insert(id);
    } else {
        m_mandatory_buckets.erase(id);
    }
    if (bucket.resident_tile_count > bucket.non_fine_tile_count) {
        m_fine_buckets.insert(id);
    } else {
        m_fine_buckets.erase(id);
    }
    bool has_non_fine_candidate = false;
    for (uint32_t index = 0U; index < DETAIL_COUNT; ++index) {
        if (index != static_cast<uint32_t>(shared::HeightTileSurfaceDetail::Fine)
            && (bucket.candidate_details & static_cast<uint8_t>(1U << index)) != 0U) {
            has_non_fine_candidate = true;
            break;
        }
    }
    if (bucket.non_fine_tile_count != 0U && has_non_fine_candidate) {
        m_candidate_buckets.insert(id);
    } else {
        m_candidate_buckets.erase(id);
    }
}

void PlayerPreviewLod::requestOptionalFineSweep() noexcept
{
    ++m_optional_sweep_epoch;
    if (m_optional_sweep_epoch == 0U) {
        m_optional_sweep_epoch = 1U;
        for (uint32_t const id : m_fine_buckets) {
            auto const bucket = m_buckets.find(id);
            if (bucket != m_buckets.end()) {
                bucket->second.optional_sweep_epoch = 0U;
            }
        }
    }
    m_optional_cursor_initialized = false;
    m_optional_fine_sweep_active = !m_fine_buckets.empty();
}

void PlayerPreviewLod::rebuildGroups(Bucket& bucket) const noexcept
{
    bucket.groups = {};
    bucket.candidate_details = 0U;
    for (auto const& tile : bucket.tiles) {
        if (!tile.has_value()) {
            continue;
        }
        Tile const& current_tile = *tile;
        Group& group = bucket.groups[static_cast<uint32_t>(current_tile.detail)];
        double const x = static_cast<double>(current_tile.key.x) * shared::HEIGHT_TILE_SIDE_LENGTH;
        double const y = static_cast<double>(current_tile.key.y) * shared::HEIGHT_TILE_SIDE_LENGTH;
        shared::HeightTileSurfaceBounds const bounds{
            x, x + shared::HEIGHT_TILE_SIDE_LENGTH,
            y, y + shared::HEIGHT_TILE_SIDE_LENGTH,
            static_cast<double>(current_tile.minimum), static_cast<double>(current_tile.maximum),
        };
        if (!group.present) {
            group.bounds = bounds;
            group.minimum_areas = current_tile.face_areas;
            group.maximum_areas = current_tile.face_areas;
            group.present = true;
        } else {
            group.bounds.min_x_blocks = std::min(group.bounds.min_x_blocks, bounds.min_x_blocks);
            group.bounds.max_x_blocks = std::max(group.bounds.max_x_blocks, bounds.max_x_blocks);
            group.bounds.min_y_blocks = std::min(group.bounds.min_y_blocks, bounds.min_y_blocks);
            group.bounds.max_y_blocks = std::max(group.bounds.max_y_blocks, bounds.max_y_blocks);
            group.bounds.min_z_blocks = std::min(group.bounds.min_z_blocks, bounds.min_z_blocks);
            group.bounds.max_z_blocks = std::max(group.bounds.max_z_blocks, bounds.max_z_blocks);
            for (uint32_t level = 0U; level < current_tile.face_areas.maximum.size(); ++level) {
                group.minimum_areas.maximum[level] = std::min(group.minimum_areas.maximum[level],
                    current_tile.face_areas.maximum[level]);
                group.maximum_areas.maximum[level] = std::max(group.maximum_areas.maximum[level],
                    current_tile.face_areas.maximum[level]);
            }
        }
    }
    bucket.dirty = false;
    bucket.certified = false;
}

PlayerPreviewLodRefresh PlayerPreviewLod::refresh(
    shared::HeightTileKey const center,
    shared::HeightTileSurfaceBounds const viewer_bounds,
    shared::HeightTileSurfaceProjection const projection
)
{
    PlayerPreviewLodRefresh result;
    bool const center_changed = !m_initialized || m_center != center;
    bool const projection_changed = !m_initialized || m_projection != projection;
    bool const viewer_changed = !m_initialized || !(m_viewer_bounds == viewer_bounds);
    if (!center_changed && !projection_changed && m_viewer_bounds == viewer_bounds
        && !m_optional_fine_sweep_active && m_dirty_buckets.empty()) {
        return result;
    }
    m_center = center;
    m_viewer_bounds = viewer_bounds;
    m_projection = projection;
    if (projection_changed) {
        m_policy = shared::HeightTileSurfaceLodPolicy{ projection };
    }
    constexpr double REGION_MARGIN = shared::HEIGHT_TILE_SIDE_LENGTH * 8.0;
    constexpr double TILE_SIDE = static_cast<double>(shared::HEIGHT_TILE_SIDE_LENGTH);
    shared::HeightTileSurfaceBounds const viewer_region{
        .min_x_blocks = viewer_bounds.min_x_blocks - REGION_MARGIN,
        .max_x_blocks = viewer_bounds.max_x_blocks + REGION_MARGIN,
        .min_y_blocks = viewer_bounds.min_y_blocks - REGION_MARGIN,
        .max_y_blocks = viewer_bounds.max_y_blocks + REGION_MARGIN,
        .min_z_blocks = viewer_bounds.min_z_blocks - REGION_MARGIN,
        .max_z_blocks = viewer_bounds.max_z_blocks + REGION_MARGIN,
    };

    int64_t const global_center_dx = m_initialized
        ? shortestTileDisplacement(m_certified_center.x, center.x)
        : 0;
    int64_t const global_center_dy = m_initialized
        ? shortestTileDisplacement(m_certified_center.y, center.y)
        : 0;
    double const global_center_offset_x = static_cast<double>(global_center_dx) * TILE_SIDE;
    double const global_center_offset_y = static_cast<double>(global_center_dy) * TILE_SIDE;
    shared::HeightTileSurfaceBounds const viewer_in_certified_frame{
        viewer_bounds.min_x_blocks + global_center_offset_x,
        viewer_bounds.max_x_blocks + global_center_offset_x,
        viewer_bounds.min_y_blocks + global_center_offset_y,
        viewer_bounds.max_y_blocks + global_center_offset_y,
        viewer_bounds.min_z_blocks,
        viewer_bounds.max_z_blocks,
    };
    bool const full_scan = projection_changed || !m_initialized
        || !centerTranslationStable(center)
        || !contains(m_certified_viewer_bounds, viewer_in_certified_frame);
    result.full_scan = full_scan;
    if (full_scan) {
        ++m_certificate_epoch;
        m_certified_center = center;
        m_certified_viewer_bounds = viewer_region;
    }
    if (full_scan || center_changed || projection_changed || viewer_changed) {
        requestOptionalFineSweep();
    }
    m_initialized = true;

    auto const groupMayChange = [&](
        shared::HeightTileSurfaceBounds const viewer,
        shared::HeightTileSurfaceBounds const bounds,
        Group const& group,
        shared::HeightTileSurfaceDetail const current
    ) {
        ++result.policy_evaluations;
        if (m_policy.detailFor(viewer, bounds, group.maximum_areas, current) != current) {
            return true;
        }
        shared::HeightTileSurfaceBounds const farthest_viewer = farthestViewer(viewer, bounds);
        shared::HeightTileSurfaceBounds const farthest = farthestSurface(farthest_viewer, bounds);
        ++result.policy_evaluations;
        return m_policy.detailFor(farthest_viewer, farthest, group.minimum_areas, current) != current;
    };

    auto const processBucket = [&](uint32_t const id, Bucket& bucket, bool const allow_fine) {
        ++result.visited_buckets;
        if (bucket.dirty) {
            rebuildGroups(bucket);
            updateBucketIndexes(id, bucket);
        }
        int64_t const center_dx = bucket.certified
            ? shortestTileDisplacement(bucket.certified_center.x, m_center.x)
            : 0;
        int64_t const center_dy = bucket.certified
            ? shortestTileDisplacement(bucket.certified_center.y, m_center.y)
            : 0;
        double const center_offset_x = static_cast<double>(center_dx) * TILE_SIDE;
        double const center_offset_y = static_cast<double>(center_dy) * TILE_SIDE;
        shared::HeightTileSurfaceBounds const viewer_in_certified_frame{
            viewer_bounds.min_x_blocks + center_offset_x,
            viewer_bounds.max_x_blocks + center_offset_x,
            viewer_bounds.min_y_blocks + center_offset_y,
            viewer_bounds.max_y_blocks + center_offset_y,
            viewer_bounds.min_z_blocks,
            viewer_bounds.max_z_blocks,
        };
        // Reuse is safe only while every tile in the bucket keeps the same wrapped image.
        bool const image_translation_stable = imageTranslationStable(
            bucket, m_center, center_offset_x, center_offset_y
        );
        bool const viewer_certificate_valid = bucket.certificate_epoch == m_certificate_epoch
            && image_translation_stable
            && contains(bucket.certified_viewer_bounds, viewer_in_certified_frame);
        if (!viewer_certificate_valid) {
            ++result.classified_buckets;
            bucket.candidate_details = 0U;
            bucket.certified_viewer_bounds = viewer_region;
            bucket.certified_center = m_center;
            for (uint32_t index = 0U; index < bucket.groups.size(); ++index) {
                Group const& group = bucket.groups[index];
                if (!group.present) {
                    continue;
                }
                shared::HeightTileSurfaceBounds const bounds = boundsInFrame(group.bounds, m_center);
                auto const current = static_cast<shared::HeightTileSurfaceDetail>(index);
                // Distance and area extrema enclose every tile's candidate selection.
                if (groupMayChange(viewer_region, bounds, group, current)) {
                    bucket.candidate_details |= static_cast<uint8_t>(1U << index);
                }
            }
            bucket.certified = true;
            bucket.certificate_epoch = m_certificate_epoch;
            if (!full_scan) {
                // Keep the shared certificate inside every bucket refreshed on this pass.
                shared::HeightTileSurfaceBounds const region_in_certified_frame{
                    viewer_region.min_x_blocks + global_center_offset_x,
                    viewer_region.max_x_blocks + global_center_offset_x,
                    viewer_region.min_y_blocks + global_center_offset_y,
                    viewer_region.max_y_blocks + global_center_offset_y,
                    viewer_region.min_z_blocks,
                    viewer_region.max_z_blocks,
                };
                m_certified_viewer_bounds.min_x_blocks = std::max(
                    m_certified_viewer_bounds.min_x_blocks, region_in_certified_frame.min_x_blocks);
                m_certified_viewer_bounds.max_x_blocks = std::min(
                    m_certified_viewer_bounds.max_x_blocks, region_in_certified_frame.max_x_blocks);
                m_certified_viewer_bounds.min_y_blocks = std::max(
                    m_certified_viewer_bounds.min_y_blocks, region_in_certified_frame.min_y_blocks);
                m_certified_viewer_bounds.max_y_blocks = std::min(
                    m_certified_viewer_bounds.max_y_blocks, region_in_certified_frame.max_y_blocks);
                m_certified_viewer_bounds.min_z_blocks = std::max(
                    m_certified_viewer_bounds.min_z_blocks, region_in_certified_frame.min_z_blocks);
                m_certified_viewer_bounds.max_z_blocks = std::min(
                    m_certified_viewer_bounds.max_z_blocks, region_in_certified_frame.max_z_blocks);
            }
        }
        updateBucketIndexes(id, bucket);
        if (bucket.candidate_details == 0U) {
            return;
        }
        uint8_t active_details = 0U;
        for (uint32_t index = 0U; index < bucket.groups.size(); ++index) {
            if ((bucket.candidate_details & (1U << index)) == 0U) {
                continue;
            }
            bool const is_fine = index == static_cast<uint32_t>(shared::HeightTileSurfaceDetail::Fine);
            if (is_fine != allow_fine) {
                continue;
            }
            Group const& group = bucket.groups[index];
            if (groupMayChange(viewer_bounds, boundsInFrame(group.bounds, m_center), group,
                static_cast<shared::HeightTileSurfaceDetail>(index))) {
                active_details |= static_cast<uint8_t>(1U << index);
            }
        }
        bool detail_changed = false;
        for (auto& tile : bucket.tiles) {
            if (!tile || (active_details & (1U << static_cast<uint32_t>(tile->detail))) == 0U) {
                continue;
            }
            ++result.evaluated_tiles;
            ++result.policy_evaluations;
            auto const selected = m_policy.detailFor(viewer_bounds, surfaceBounds(*tile),
                tile->face_areas, tile->detail);
            if (selected != tile->detail) {
                if (tile->detail == shared::HeightTileSurfaceDetail::Fine) {
                    ++bucket.non_fine_tile_count;
                } else if (selected == shared::HeightTileSurfaceDetail::Fine) {
                    --bucket.non_fine_tile_count;
                }
                tile->detail = selected;
                result.changed_keys.push_back(tile->key);
                detail_changed = true;
            }
        }
        if (detail_changed) {
            bucket.dirty = true;
            bucket.certified = false;
            bucket.candidate_details = 0U;
            m_dirty_buckets.insert(id);
            updateBucketIndexes(id, bucket);
        }
    };

    std::vector<uint32_t> affected_buckets;
    if (full_scan) {
        // Mandatory processing updates candidate membership in place, avoiding a global node-free/reinsert burst.
        affected_buckets.reserve(m_mandatory_buckets.size());
        affected_buckets.insert(affected_buckets.end(),
            m_mandatory_buckets.begin(), m_mandatory_buckets.end());
    } else {
        affected_buckets.reserve(m_dirty_buckets.size()
            + (center_changed || projection_changed || viewer_changed ? m_candidate_buckets.size() : 0U));
        if (center_changed || projection_changed || viewer_changed) {
            affected_buckets.insert(affected_buckets.end(), m_candidate_buckets.begin(), m_candidate_buckets.end());
        }
        affected_buckets.insert(affected_buckets.end(), m_dirty_buckets.begin(), m_dirty_buckets.end());
        std::ranges::sort(affected_buckets);
        auto const unique_end = std::ranges::unique(affected_buckets);
        affected_buckets.erase(unique_end.begin(), unique_end.end());
    }
    m_dirty_buckets.clear();
    for (uint32_t const id : affected_buckets) {
        auto const bucket = m_buckets.find(id);
        if (bucket != m_buckets.end()) {
            processBucket(id, bucket->second, false);
        }
    }

    while (m_optional_fine_sweep_active
        && result.optional_buckets_inspected < MAX_OPTIONAL_FINE_BUCKETS_PER_REFRESH
        && !m_fine_buckets.empty()) {
        auto fine_bucket = m_optional_cursor_initialized
            ? m_fine_buckets.upper_bound(m_optional_cursor_id)
            : m_fine_buckets.begin();
        if (fine_bucket == m_fine_buckets.end()) {
            fine_bucket = m_fine_buckets.begin();
        }
        uint32_t const id = *fine_bucket;
        m_optional_cursor_id = id;
        m_optional_cursor_initialized = true;
        ++result.optional_buckets_inspected;
        auto const bucket = m_buckets.find(id);
        if (bucket == m_buckets.end()) {
            m_fine_buckets.erase(fine_bucket);
            continue;
        }
        if (bucket->second.optional_sweep_epoch == m_optional_sweep_epoch) {
            m_optional_fine_sweep_active = false;
            m_optional_cursor_initialized = false;
            break;
        }
        bucket->second.optional_sweep_epoch = m_optional_sweep_epoch;
        processBucket(id, bucket->second, true);
    }
    if (m_fine_buckets.empty()) {
        m_optional_fine_sweep_active = false;
        m_optional_cursor_initialized = false;
    }
    return result;
}

} // namespace client
