#include <client/PlayerPreviewLod.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <core/common/Assert.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
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

void PlayerPreviewLod::clear() noexcept
{
    m_buckets.clear();
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
    auto const bucket = m_buckets.find(bucketId(key));
    if (bucket == m_buckets.end()) {
        return;
    }
    bucket->second.tiles[tileIndex(key)].reset();
    bucket->second.dirty = true;
    if (std::ranges::none_of(bucket->second.tiles, [](auto const& tile) { return tile.has_value(); })) {
        m_buckets.erase(bucket);
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
    }
    previous = next;
    return next.detail;
}

void PlayerPreviewLod::rebuildGroups(Bucket& bucket) const noexcept
{
    bucket.groups = {};
    for (auto const& tile : bucket.tiles) {
        if (!tile) {
            continue;
        }
        Group& group = bucket.groups[static_cast<uint32_t>(tile->detail)];
        double const x = static_cast<double>(tile->key.x) * shared::HEIGHT_TILE_SIDE_LENGTH;
        double const y = static_cast<double>(tile->key.y) * shared::HEIGHT_TILE_SIDE_LENGTH;
        shared::HeightTileSurfaceBounds const bounds{
            x, x + shared::HEIGHT_TILE_SIDE_LENGTH,
            y, y + shared::HEIGHT_TILE_SIDE_LENGTH,
            static_cast<double>(tile->minimum), static_cast<double>(tile->maximum),
        };
        if (!group.present) {
            group.bounds = bounds;
            group.minimum_areas = tile->face_areas;
            group.maximum_areas = tile->face_areas;
            group.present = true;
        } else {
            group.bounds.min_x_blocks = std::min(group.bounds.min_x_blocks, bounds.min_x_blocks);
            group.bounds.max_x_blocks = std::max(group.bounds.max_x_blocks, bounds.max_x_blocks);
            group.bounds.min_y_blocks = std::min(group.bounds.min_y_blocks, bounds.min_y_blocks);
            group.bounds.max_y_blocks = std::max(group.bounds.max_y_blocks, bounds.max_y_blocks);
            group.bounds.min_z_blocks = std::min(group.bounds.min_z_blocks, bounds.min_z_blocks);
            group.bounds.max_z_blocks = std::max(group.bounds.max_z_blocks, bounds.max_z_blocks);
            for (uint32_t level = 0U; level < tile->face_areas.maximum.size(); ++level) {
                group.minimum_areas.maximum[level] = std::min(group.minimum_areas.maximum[level],
                    tile->face_areas.maximum[level]);
                group.maximum_areas.maximum[level] = std::max(group.maximum_areas.maximum[level],
                    tile->face_areas.maximum[level]);
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
    if (!center_changed && !projection_changed && m_viewer_bounds == viewer_bounds) {
        return result;
    }
    m_center = center;
    m_viewer_bounds = viewer_bounds;
    m_projection = projection;
    if (projection_changed) {
        m_policy = shared::HeightTileSurfaceLodPolicy{ projection };
    }
    m_initialized = true;
    constexpr double REGION_MARGIN = shared::HEIGHT_TILE_SIDE_LENGTH;
    shared::HeightTileSurfaceBounds const viewer_region{
        .min_x_blocks = viewer_bounds.min_x_blocks - REGION_MARGIN,
        .max_x_blocks = viewer_bounds.max_x_blocks + REGION_MARGIN,
        .min_y_blocks = viewer_bounds.min_y_blocks - REGION_MARGIN,
        .max_y_blocks = viewer_bounds.max_y_blocks + REGION_MARGIN,
        .min_z_blocks = viewer_bounds.min_z_blocks - REGION_MARGIN,
        .max_z_blocks = viewer_bounds.max_z_blocks + REGION_MARGIN,
    };
    for (auto& [id, bucket] : m_buckets) {
        static_cast<void>(id);
        if (bucket.dirty) {
            rebuildGroups(bucket);
        }
        if (center_changed || projection_changed || !bucket.certified
            || !contains(bucket.certified_viewer_bounds, viewer_bounds)) {
            ++result.classified_buckets;
            bucket.candidate_details = 0U;
            bucket.certified_viewer_bounds = viewer_region;
            for (uint32_t index = 0U; index < bucket.groups.size(); ++index) {
                Group const& group = bucket.groups[index];
                if (!group.present) {
                    continue;
                }
                shared::HeightTileSurfaceBounds bounds = group.bounds;
                shared::HeightTileSurfaceBounds const origin = shared::HeightTileSurfaceMesher::boundsForTile(
                    { m_center.x, m_center.y },
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
                auto const current = static_cast<shared::HeightTileSurfaceDetail>(index);
                shared::HeightTileSurfaceBounds const farthest_viewer = farthestViewer(viewer_region, bounds);
                shared::HeightTileSurfaceBounds const farthest = farthestSurface(farthest_viewer, bounds);
                // Distance and area extrema enclose every tile's candidate selection.
                if (m_policy.detailFor(viewer_region, bounds, group.maximum_areas, current) != current
                    || m_policy.detailFor(farthest_viewer, farthest, group.minimum_areas, current) != current) {
                    bucket.candidate_details |= static_cast<uint8_t>(1U << index);
                }
            }
            bucket.certified = true;
        }
        if (bucket.candidate_details == 0U) {
            continue;
        }
        for (auto& tile : bucket.tiles) {
            if (!tile || (bucket.candidate_details & (1U << static_cast<uint32_t>(tile->detail))) == 0U) {
                continue;
            }
            ++result.evaluated_tiles;
            auto const selected = m_policy.detailFor(viewer_bounds, surfaceBounds(*tile),
                tile->face_areas, tile->detail);
            if (selected != tile->detail) {
                tile->detail = selected;
                result.changed_keys.push_back(tile->key);
                bucket.dirty = true;
            }
        }
    }
    return result;
}

} // namespace client
