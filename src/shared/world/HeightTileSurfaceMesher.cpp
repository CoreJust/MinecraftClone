#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <shared/world/SparseWorld.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace {

constexpr uint32_t SIDE_LENGTH = shared::HeightTile::SIDE_LENGTH;
constexpr uint32_t COARSE2_CELL_SIZE = 2U;
constexpr uint32_t COARSE4_CELL_SIZE = 4U;
constexpr uint32_t COARSE_CELL_SIZE = 8U;
constexpr uint32_t DISTANT_CELL_SIZE = SIDE_LENGTH;
constexpr double DEGREES_TO_RADIANS = 0.017'453'292'519'943'295'769;
constexpr double MAXIMUM_SIMPLIFIED_FACE_AREA_PIXELS_SQUARED = 2.0;
constexpr double COARSENING_HYSTERESIS_RATIO = 1.1;
constexpr int32_t WORLD_TILE_COUNT = static_cast<int32_t>(
    shared::WorldExtent::WIDTH / shared::HeightTile::SIDE_LENGTH
);

[[nodiscard]]
constexpr uint32_t index(uint32_t const x, uint32_t const y) noexcept
{
    return y * SIDE_LENGTH + x;
}

uint32_t detailCellSize(shared::HeightTileSurfaceDetail detail);

struct MaterialRun final {
    int32_t lower = 0;
    int32_t upper = 0;
    shared::Block material = shared::Block::Stone;

    constexpr bool operator==(MaterialRun const&) const noexcept = default;
};

struct MaterialProfile final {
    std::array<MaterialRun, shared::HeightTileSurfaceMesh::MAXIMUM_MATERIAL_RUNS_PER_COLUMN> runs{};
    uint32_t count = 0U;

    constexpr bool operator==(MaterialProfile const&) const noexcept = default;
};

[[nodiscard]]
bool hasCanonicalSurface(shared::HeightTile const& tile) noexcept
{
    return shared::isValidTerrainIdentity(tile.surface.identity)
        && tile.surface.coordinate.x == tile.coordinate.x
        && tile.surface.coordinate.y == tile.coordinate.y
        && tile.surface.coordinate.z == 0
        && tile.surface.block_sea_level < shared::WorldExtent::DEPTH;
}

[[nodiscard]]
shared::TerrainColumn columnAtReducedHeight(
    shared::TerrainColumn column,
    uint16_t const highest_solid_z,
    uint32_t const sea_level
) noexcept
{
    column.highest_solid_z = highest_solid_z;
    column.submerged = highest_solid_z < sea_level;
    uint32_t const ground_first = column.submerged
        ? (sea_level > 4U ? sea_level - 4U : 1U)
        : (highest_solid_z > 4U ? static_cast<uint32_t>(highest_solid_z) - 4U : 1U);
    column.ground_first_z = static_cast<uint16_t>(std::max(ground_first, 1U));
    uint32_t const ground_end = column.submerged
        ? static_cast<uint32_t>(highest_solid_z) + 1U
        : highest_solid_z;
    column.ground_end_z = static_cast<uint16_t>(std::min(
        std::max(ground_end, ground_first),
        generation::WORLD_DEPTH_BLOCKS
    ));
    return column;
}

[[nodiscard]]
shared::Block surfaceMaterialAt(
    shared::SurfaceTile const* const surface,
    shared::HeightTileSurfaceMesh const& mesh,
    shared::HeightTileSurfaceDirection const edge,
    uint32_t const along,
    int32_t const top_height,
    int32_t const z
)
{
    if (surface == nullptr
        || !shared::isValidTerrainIdentity(surface->identity)
        || surface->identity != mesh.identity
        || surface->coordinate.x != mesh.coordinate.x
        || surface->coordinate.y != mesh.coordinate.y
        || surface->coordinate.z != 0
        || surface->block_sea_level >= shared::WorldExtent::DEPTH
        || top_height <= 0 || top_height > static_cast<int32_t>(shared::WorldExtent::DEPTH)) {
        return shared::Block::Stone;
    }
    uint32_t const cell_size = detailCellSize(mesh.detail);
    bool const x_edge = edge == shared::HeightTileSurfaceDirection::NegativeX
        || edge == shared::HeightTileSurfaceDirection::PositiveX;
    bool const negative_edge = edge == shared::HeightTileSurfaceDirection::NegativeX
        || edge == shared::HeightTileSurfaceDirection::NegativeY;
    uint32_t const cell_perpendicular = negative_edge
        ? 0U
        : SIDE_LENGTH - cell_size;
    uint32_t const source_x = x_edge
        ? cell_perpendicular + cell_size / 2U
        : (along / cell_size) * cell_size + cell_size / 2U;
    uint32_t const source_y = x_edge
        ? (along / cell_size) * cell_size + cell_size / 2U
        : cell_perpendicular + cell_size / 2U;
    uint32_t const column_index = index(source_x, source_y);
    shared::TerrainColumn const column = columnAtReducedHeight(
        surface->columns[column_index],
        static_cast<uint16_t>(top_height - 1),
        surface->block_sea_level
    );
    if (z < 0 || z > column.highest_solid_z) {
        return shared::Block::Stone;
    }
    return column.materialAt(static_cast<uint16_t>(z), surface->block_sea_level);
}

[[nodiscard]]
shared::Block materialAt(
    shared::HeightTile const& tile,
    uint32_t const column_index,
    int32_t const z,
    bool const canonical_surface
) noexcept
{
    if (!canonical_surface) {
        return shared::Block::Stone;
    }
    shared::TerrainColumn const& column = tile.surface.columns[column_index];
    if (static_cast<uint32_t>(column.highest_solid_z) + 1U != tile.heights[column_index]
        || z < 0 || z > column.highest_solid_z) {
        return shared::Block::Stone;
    }
    return column.materialAt(static_cast<uint16_t>(z), tile.surface.block_sea_level);
}

[[nodiscard]]
MaterialProfile materialProfile(
    shared::HeightTile const& tile,
    uint32_t const column_index,
    int32_t const lower,
    int32_t const upper,
    bool const canonical_surface
)
{
    MaterialProfile profile;
    for (int32_t z = lower; z < upper; ++z) {
        shared::Block const material = materialAt(tile, column_index, z, canonical_surface);
        if (profile.count != 0U && profile.runs[profile.count - 1U].material == material) {
            profile.runs[profile.count - 1U].upper = z + 1;
            continue;
        }
        if (profile.count == profile.runs.size()) {
            throw std::invalid_argument{ "terrain column has too many material layers" };
        }
        profile.runs[profile.count++] = { .lower = z, .upper = z + 1, .material = material };
    }
    return profile;
}

[[nodiscard]]
constexpr int32_t worldCoordinate(int32_t const tile_coordinate, uint32_t const local_coordinate) noexcept
{
    return tile_coordinate * static_cast<int32_t>(SIDE_LENGTH) + static_cast<int32_t>(local_coordinate);
}

[[nodiscard]]
std::optional<uint16_t> heightAt(
    shared::HeightTile const& tile,
    shared::HeightTileSurfaceNeighbors const& neighbors,
    int32_t const x,
    int32_t const y
) noexcept {
    if (x >= 0 && x < static_cast<int32_t>(SIDE_LENGTH)
        && y >= 0 && y < static_cast<int32_t>(SIDE_LENGTH)) {
        return tile.heights[index(static_cast<uint32_t>(x), static_cast<uint32_t>(y))];
    }
    if (x < 0 && neighbors.negative_x.has_value()) {
        return (*neighbors.negative_x)[index(SIDE_LENGTH - 1U, static_cast<uint32_t>(y))];
    }
    if (x >= static_cast<int32_t>(SIDE_LENGTH) && neighbors.positive_x.has_value()) {
        return (*neighbors.positive_x)[index(0U, static_cast<uint32_t>(y))];
    }
    if (y < 0 && neighbors.negative_y.has_value()) {
        return (*neighbors.negative_y)[index(static_cast<uint32_t>(x), SIDE_LENGTH - 1U)];
    }
    if (y >= static_cast<int32_t>(SIDE_LENGTH) && neighbors.positive_y.has_value()) {
        return (*neighbors.positive_y)[index(static_cast<uint32_t>(x), 0U)];
    }
    return std::nullopt;
}

void appendTopQuads(
    shared::HeightTileSurfaceMesh& mesh,
    shared::HeightTile const& tile,
    bool const canonical_surface
)
{
    std::array<bool, shared::HeightTile::SAMPLE_COUNT> consumed{};
    for (uint32_t y = 0U; y < SIDE_LENGTH; ++y) {
        for (uint32_t x = 0U; x < SIDE_LENGTH; ++x) {
            uint32_t const start = index(x, y);
            uint16_t const height = tile.heights[start];
            if (height == 0U || consumed[start]) {
                continue;
            }
            shared::Block const material = materialAt(tile, start, static_cast<int32_t>(height) - 1,
                canonical_surface);
            uint32_t width = 1U;
            while (x + width < SIDE_LENGTH && !consumed[index(x + width, y)]
                && tile.heights[index(x + width, y)] == height
                && materialAt(tile, index(x + width, y), static_cast<int32_t>(height) - 1,
                    canonical_surface) == material) {
                ++width;
            }
            uint32_t depth = 1U;
            bool matches = true;
            while (y + depth < SIDE_LENGTH && matches) {
                for (uint32_t column = 0U; column < width; ++column) {
                    uint32_t const candidate = index(x + column, y + depth);
                    if (consumed[candidate] || tile.heights[candidate] != height
                        || materialAt(tile, candidate, static_cast<int32_t>(height) - 1,
                            canonical_surface) != material) {
                        matches = false;
                        break;
                    }
                }
                if (matches) {
                    ++depth;
                }
            }
            for (uint32_t row = 0U; row < depth; ++row) {
                for (uint32_t column = 0U; column < width; ++column) {
                    consumed[index(x + column, y + row)] = true;
                }
            }
            mesh.quads.push_back({
                .x = worldCoordinate(tile.coordinate.x, x),
                .y = worldCoordinate(tile.coordinate.y, y),
                .z = static_cast<int32_t>(height) - 1,
                .direction = shared::HeightTileSurfaceDirection::PositiveZ,
                .u_extent = static_cast<uint16_t>(width),
                .v_extent = static_cast<uint16_t>(depth),
                .material = material,
            });
        }
    }
}

int64_t shortestTileDisplacement(int32_t const from, int32_t const to) noexcept
{
    int64_t displacement = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    if (displacement > WORLD_TILE_COUNT / 2) {
        displacement -= WORLD_TILE_COUNT;
    } else if (displacement < -WORLD_TILE_COUNT / 2) {
        displacement += WORLD_TILE_COUNT;
    }
    return displacement;
}

void appendWallQuads(
    shared::HeightTileSurfaceMesh& mesh,
    shared::HeightTile const& tile,
    shared::HeightTileSurfaceNeighbors const& neighbors,
    shared::HeightTileSurfaceDirection const direction,
    bool const canonical_surface
)
{
    bool const x_wall = direction == shared::HeightTileSurfaceDirection::NegativeX
        || direction == shared::HeightTileSurfaceDirection::PositiveX;
    bool const negative = direction == shared::HeightTileSurfaceDirection::NegativeX
        || direction == shared::HeightTileSurfaceDirection::NegativeY;
    int32_t const offset = negative ? -1 : 1;
    for (uint32_t perpendicular = 0U; perpendicular < SIDE_LENGTH; ++perpendicular) {
        uint32_t along = 0U;
        while (along < SIDE_LENGTH) {
            uint32_t const x = x_wall ? perpendicular : along;
            uint32_t const y = x_wall ? along : perpendicular;
            uint32_t const column_index = index(x, y);
            uint16_t const height = tile.heights[column_index];
            std::optional<uint16_t> const adjacent = heightAt(
                tile,
                neighbors,
                static_cast<int32_t>(x) + (x_wall ? offset : 0),
                static_cast<int32_t>(y) + (x_wall ? 0 : offset)
            );
            if (!adjacent.has_value() || height <= *adjacent) {
                ++along;
                continue;
            }

            MaterialProfile const profile = materialProfile(
                tile,
                column_index,
                *adjacent,
                height,
                canonical_surface
            );
            uint32_t span = 1U;
            while (along + span < SIDE_LENGTH) {
                uint32_t const next_x = x_wall ? perpendicular : along + span;
                uint32_t const next_y = x_wall ? along + span : perpendicular;
                uint32_t const next_index = index(next_x, next_y);
                uint16_t const next_height = tile.heights[next_index];
                std::optional<uint16_t> const next_adjacent = heightAt(
                    tile,
                    neighbors,
                    static_cast<int32_t>(next_x) + (x_wall ? offset : 0),
                    static_cast<int32_t>(next_y) + (x_wall ? 0 : offset)
                );
                if (!next_adjacent.has_value() || next_height != height || *next_adjacent != *adjacent
                    || materialProfile(tile, next_index, *next_adjacent, next_height, canonical_surface) != profile) {
                    break;
                }
                ++span;
            }

            for (uint32_t run = 0U; run < profile.count; ++run) {
                MaterialRun const material_run = profile.runs[run];
                mesh.quads.push_back({
                    .x = worldCoordinate(tile.coordinate.x, x),
                    .y = worldCoordinate(tile.coordinate.y, y),
                    .z = material_run.lower,
                    .direction = direction,
                    .u_extent = static_cast<uint16_t>(span),
                    .v_extent = static_cast<uint16_t>(material_run.upper - material_run.lower),
                    .material = material_run.material,
                });
            }
            along += span;
        }
    }
}

shared::HeightTile reduceTile(shared::HeightTile const& tile, uint32_t const cell_size)
{
    shared::HeightTile coarse_tile{
        .coordinate = tile.coordinate,
        .heights = {},
        .surface = tile.surface,
    };
    bool const canonical_surface = hasCanonicalSurface(tile);
    for (uint32_t coarse_y = 0U; coarse_y < SIDE_LENGTH; coarse_y += cell_size) {
        for (uint32_t coarse_x = 0U; coarse_x < SIDE_LENGTH; coarse_x += cell_size) {
            uint32_t height_sum = 0U;
            for (uint32_t y = coarse_y; y < coarse_y + cell_size; ++y) {
                for (uint32_t x = coarse_x; x < coarse_x + cell_size; ++x) {
                    height_sum += tile.heights[index(x, y)];
                }
            }
            uint32_t const cell_area = cell_size * cell_size;
            uint16_t const height = static_cast<uint16_t>(
                (height_sum + cell_area / 2U) / cell_area
            );
            uint32_t const representative_index = index(
                coarse_x + cell_size / 2U,
                coarse_y + cell_size / 2U
            );
            shared::TerrainColumn reduced_column = tile.surface.columns[representative_index];
            if (canonical_surface
                && static_cast<uint32_t>(reduced_column.highest_solid_z) + 1U == tile.heights[representative_index]
                && height != 0U) {
                reduced_column = columnAtReducedHeight(
                    std::move(reduced_column),
                    static_cast<uint16_t>(height - 1U),
                    tile.surface.block_sea_level
                );
            }
            for (uint32_t y = coarse_y; y < coarse_y + cell_size; ++y) {
                for (uint32_t x = coarse_x; x < coarse_x + cell_size; ++x) {
                    uint32_t const coarse_index = index(x, y);
                    coarse_tile.heights[coarse_index] = height;
                    coarse_tile.surface.columns[coarse_index] = reduced_column;
                }
            }
        }
    }
    return coarse_tile;
}

uint32_t detailCellSize(shared::HeightTileSurfaceDetail const detail)
{
    switch (detail) {
    case shared::HeightTileSurfaceDetail::Fine:
        return 1U;
    case shared::HeightTileSurfaceDetail::Coarse2:
        return COARSE2_CELL_SIZE;
    case shared::HeightTileSurfaceDetail::Coarse4:
        return COARSE4_CELL_SIZE;
    case shared::HeightTileSurfaceDetail::Coarse:
        return COARSE_CELL_SIZE;
    case shared::HeightTileSurfaceDetail::Distant:
        return DISTANT_CELL_SIZE;
    default:
        throw std::invalid_argument{ "height tile neighbor has an invalid detail level" };
    }
}

shared::HeightTileSurfaceNeighbors neighborsAtSelectedDetail(
    shared::HeightTileSurfaceNeighbors const& neighbors
)
{
    auto const reduceNeighbor = [](
        std::optional<shared::HeightTile::Heights> const& heights,
        shared::HeightTileSurfaceDetail const detail
    ) -> std::optional<shared::HeightTile::Heights> {
        if (!heights.has_value()) {
            return std::nullopt;
        }
        uint32_t const cell_size = detailCellSize(detail);
        if (cell_size == 1U) {
            return heights;
        }
        shared::HeightTile neighbor{};
        neighbor.heights = *heights;
        return reduceTile(neighbor, cell_size).heights;
    };
    return {
        .negative_x = reduceNeighbor(neighbors.negative_x, neighbors.negative_x_detail),
        .positive_x = reduceNeighbor(neighbors.positive_x, neighbors.positive_x_detail),
        .negative_y = reduceNeighbor(neighbors.negative_y, neighbors.negative_y_detail),
        .positive_y = reduceNeighbor(neighbors.positive_y, neighbors.positive_y_detail),
    };
}

void appendReducedQuads(
    shared::HeightTileSurfaceMesh& mesh,
    shared::HeightTile const& tile,
    shared::HeightTileSurfaceNeighbors const& neighbors,
    uint32_t const cell_size
)
{
    shared::HeightTile const coarse_tile = reduceTile(tile, cell_size);
    bool const canonical_surface = hasCanonicalSurface(coarse_tile);
    appendTopQuads(mesh, coarse_tile, canonical_surface);
    appendWallQuads(mesh, coarse_tile, neighbors, shared::HeightTileSurfaceDirection::NegativeX, canonical_surface);
    appendWallQuads(mesh, coarse_tile, neighbors, shared::HeightTileSurfaceDirection::PositiveX, canonical_surface);
    appendWallQuads(mesh, coarse_tile, neighbors, shared::HeightTileSurfaceDirection::NegativeY, canonical_surface);
    appendWallQuads(mesh, coarse_tile, neighbors, shared::HeightTileSurfaceDirection::PositiveY, canonical_surface);
}

[[nodiscard]]
uint32_t reducedQuadReservation(uint32_t const cell_size) noexcept
{
    uint32_t const coarse_side_length = SIDE_LENGTH / cell_size;
    return coarse_side_length * coarse_side_length
        + 4U * SIDE_LENGTH * coarse_side_length;
}

shared::HeightTileSurfaceDirection oppositeDirection(shared::HeightTileSurfaceDirection const direction)
{
    switch (direction) {
    case shared::HeightTileSurfaceDirection::NegativeX:
        return shared::HeightTileSurfaceDirection::PositiveX;
    case shared::HeightTileSurfaceDirection::PositiveX:
        return shared::HeightTileSurfaceDirection::NegativeX;
    case shared::HeightTileSurfaceDirection::NegativeY:
        return shared::HeightTileSurfaceDirection::PositiveY;
    case shared::HeightTileSurfaceDirection::PositiveY:
        return shared::HeightTileSurfaceDirection::NegativeY;
    default:
        throw std::invalid_argument{ "height tile seam requires a horizontal edge" };
    }
}

struct VerticalInterval final {
    int32_t lower;
    int32_t upper;
};

struct EdgeSurfaceProfile final {
    std::array<int32_t, shared::HeightTile::SIDE_LENGTH> top{};
    std::array<std::vector<VerticalInterval>, shared::HeightTile::SIDE_LENGTH> walls;
};

EdgeSurfaceProfile edgeSurfaceProfile(
    shared::HeightTileSurfaceMesh const& mesh,
    shared::HeightTileSurfaceDirection const edge
)
{
    bool const x_edge = edge == shared::HeightTileSurfaceDirection::NegativeX
        || edge == shared::HeightTileSurfaceDirection::PositiveX;
    uint32_t const edge_offset = edge == shared::HeightTileSurfaceDirection::NegativeX
            || edge == shared::HeightTileSurfaceDirection::NegativeY
        ? 0U
        : static_cast<uint32_t>(shared::HeightTile::SIDE_LENGTH - 1U);
    int64_t const origin_x = static_cast<int64_t>(mesh.coordinate.x) * SIDE_LENGTH;
    int64_t const origin_y = static_cast<int64_t>(mesh.coordinate.y) * SIDE_LENGTH;
    int64_t const boundary = (x_edge ? origin_x : origin_y) + edge_offset;
    int64_t const along_origin = x_edge ? origin_y : origin_x;
    EdgeSurfaceProfile profile;
    for (shared::HeightTileSurfaceQuad const& quad : mesh.quads) {
        bool const top = quad.direction == shared::HeightTileSurfaceDirection::PositiveZ;
        if (!top && quad.direction != edge) {
            continue;
        }
        int64_t const perpendicular = x_edge ? quad.x : quad.y;
        if (top) {
            int64_t const perpendicular_extent = x_edge ? quad.u_extent : quad.v_extent;
            if (boundary < perpendicular || boundary >= perpendicular + perpendicular_extent) {
                continue;
            }
        } else if (perpendicular != boundary) {
            continue;
        }
        int64_t const along_start = static_cast<int64_t>(x_edge ? quad.y : quad.x) - along_origin;
        int64_t const along_extent = top && x_edge ? quad.v_extent : quad.u_extent;
        int64_t const clipped_start = std::max<int64_t>(0, along_start);
        int64_t const clipped_end = std::min<int64_t>(SIDE_LENGTH, along_start + along_extent);
        for (int64_t along = clipped_start; along < clipped_end; ++along) {
            uint32_t const sample = static_cast<uint32_t>(along);
            if (top) {
                profile.top[sample] = quad.z + 1;
            } else {
                profile.walls[sample].push_back({ quad.z, quad.z + quad.v_extent });
            }
        }
    }
    return profile;
}

} // namespace

namespace shared {

HeightTileSurfaceLodPolicy::HeightTileSurfaceLodPolicy(
    HeightTileSurfaceProjection const projection
) noexcept
{
    if (!std::isfinite(projection.vertical_fov_degrees)
        || projection.vertical_fov_degrees <= 0.0
        || projection.vertical_fov_degrees >= 180.0
        || projection.viewport_width_pixels == 0U
        || projection.viewport_height_pixels == 0U) {
        return;
    }

    double const vertical_tangent = std::tan(
        projection.vertical_fov_degrees * DEGREES_TO_RADIANS / 2.0
    );
    double const horizontal_tangent = vertical_tangent
        * static_cast<double>(projection.viewport_width_pixels)
        / static_cast<double>(projection.viewport_height_pixels);
    double const viewport_scale_squared = 1.0
        + horizontal_tangent * horizontal_tangent
        + vertical_tangent * vertical_tangent;
    double const focal_length_pixels = static_cast<double>(projection.viewport_height_pixels)
        / (2.0 * vertical_tangent);
    // The corner bound is scaled by each replacement cell's largest possible face area.
    m_first_reduction_distance = focal_length_pixels
        * std::pow(viewport_scale_squared, 0.75)
        / std::sqrt(MAXIMUM_SIMPLIFIED_FACE_AREA_PIXELS_SQUARED);
    if (!std::isfinite(m_first_reduction_distance) || m_first_reduction_distance <= 0.0) {
        m_first_reduction_distance = 0.0;
    }
}

HeightTileSurfaceDetail HeightTileSurfaceLodPolicy::detailFor(
    HeightTileSurfaceBounds const viewer_bounds,
    HeightTileSurfaceBounds const surface_bounds,
    std::optional<HeightTileSurfaceDetail> const current_detail
) const noexcept
{
    HeightTileSurfaceFaceAreas face_areas;
    double const surface_height = surface_bounds.max_z_blocks - surface_bounds.min_z_blocks;
    for (uint32_t level = 0U; level < face_areas.maximum.size(); ++level) {
        double const cell_size = HeightTileSurfaceFaceAreas::CELL_SIZES[level];
        face_areas.maximum[level] = std::max(cell_size * cell_size, cell_size * surface_height);
    }
    return detailFor(viewer_bounds, surface_bounds, face_areas, current_detail);
}

HeightTileSurfaceDetail HeightTileSurfaceLodPolicy::detailFor(
    HeightTileSurfaceBounds const viewer_bounds,
    HeightTileSurfaceBounds const surface_bounds,
    HeightTileSurfaceFaceAreas const& face_areas,
    std::optional<HeightTileSurfaceDetail> const current_detail
) const noexcept
{
    auto const validBounds = [](HeightTileSurfaceBounds const bounds) noexcept {
        return std::isfinite(bounds.min_x_blocks) && std::isfinite(bounds.max_x_blocks)
            && std::isfinite(bounds.min_y_blocks) && std::isfinite(bounds.max_y_blocks)
            && std::isfinite(bounds.min_z_blocks) && std::isfinite(bounds.max_z_blocks)
            && bounds.min_x_blocks <= bounds.max_x_blocks
            && bounds.min_y_blocks <= bounds.max_y_blocks
            && bounds.min_z_blocks <= bounds.max_z_blocks;
    };
    if (!validBounds(viewer_bounds) || !validBounds(surface_bounds)
        || m_first_reduction_distance <= 0.0) {
        return HeightTileSurfaceDetail::Fine;
    }
    for (uint32_t level = 0U; level < face_areas.maximum.size(); ++level) {
        double const cell_size = HeightTileSurfaceFaceAreas::CELL_SIZES[level];
        if (!std::isfinite(face_areas.maximum[level])
            || face_areas.maximum[level] < cell_size * cell_size) {
            return HeightTileSurfaceDetail::Fine;
        }
    }

    auto const axisSeparation = [](
        double const first_min, // NOLINT(bugprone-easily-swappable-parameters) -- first interval bounds are ordered
        double const first_max,
        double const second_min, // NOLINT(bugprone-easily-swappable-parameters) -- second interval bounds are ordered
        double const second_max
    ) noexcept {
        if (first_max < second_min) {
            return second_min - first_max;
        }
        if (second_max < first_min) {
            return first_min - second_max;
        }
        return 0.0;
    };
    double const dx = axisSeparation(
        viewer_bounds.min_x_blocks,
        viewer_bounds.max_x_blocks,
        surface_bounds.min_x_blocks,
        surface_bounds.max_x_blocks
    );
    double const dy = axisSeparation(
        viewer_bounds.min_y_blocks,
        viewer_bounds.max_y_blocks,
        surface_bounds.min_y_blocks,
        surface_bounds.max_y_blocks
    );
    double const dz = axisSeparation(
        viewer_bounds.min_z_blocks,
        viewer_bounds.max_z_blocks,
        surface_bounds.min_z_blocks,
        surface_bounds.max_z_blocks
    );
    double const distance = std::hypot(dx, dy, dz);
    if (!std::isfinite(distance)) {
        return HeightTileSurfaceDetail::Fine;
    }
    uint32_t current_cell_size = 1U;
    if (current_detail.has_value()) {
        auto const current = std::ranges::find(HeightTileSurfaceFaceAreas::DETAILS, *current_detail);
        if (current != HeightTileSurfaceFaceAreas::DETAILS.end()) {
            current_cell_size = HeightTileSurfaceFaceAreas::CELL_SIZES[
                static_cast<uint32_t>(current - HeightTileSurfaceFaceAreas::DETAILS.begin())
            ];
        }
    }
    HeightTileSurfaceDetail selected = HeightTileSurfaceDetail::Fine;
    for (uint32_t level = 0U; level < face_areas.maximum.size(); ++level) {
        double const cutoff = m_first_reduction_distance * std::sqrt(face_areas.maximum[level]);
        bool const coarsening = current_detail.has_value()
            && HeightTileSurfaceFaceAreas::CELL_SIZES[level] > current_cell_size;
        double const required_distance = coarsening ? cutoff * COARSENING_HYSTERESIS_RATIO : cutoff;
        if (distance >= required_distance) {
            selected = HeightTileSurfaceFaceAreas::DETAILS[level];
        }
    }
    return selected;
}

HeightTileSurfaceDetail HeightTileSurfaceMesher::detailFor(
    HeightTileSurfaceBounds const viewer_bounds,
    HeightTileSurfaceBounds const surface_bounds,
    HeightTileSurfaceProjection const projection
) noexcept
{
    return HeightTileSurfaceLodPolicy{ projection }.detailFor(viewer_bounds, surface_bounds);
}

HeightTileSurfaceBounds HeightTileSurfaceMesher::boundsForTile(
    HeightTileCoordinate const player_tile,
    HeightTileCoordinate const tile,
    double const min_z_blocks,
    double const max_z_blocks
) noexcept
{
    int64_t const x = shortestTileDisplacement(player_tile.x, tile.x);
    int64_t const y = shortestTileDisplacement(player_tile.y, tile.y);
    double const side_length = static_cast<double>(SIDE_LENGTH);
    return {
        .min_x_blocks = static_cast<double>(x) * side_length,
        .max_x_blocks = static_cast<double>(x + 1) * side_length,
        .min_y_blocks = static_cast<double>(y) * side_length,
        .max_y_blocks = static_cast<double>(y + 1) * side_length,
        .min_z_blocks = min_z_blocks,
        .max_z_blocks = max_z_blocks,
    };
}

HeightTileSurfaceBounds HeightTileSurfaceMesher::boundsForTile(
    HeightTileCoordinate const player_tile,
    HeightTile const& tile,
    HeightTileSurfaceNeighbors const& neighbors
) noexcept
{
    uint16_t min_height = static_cast<uint16_t>(WorldExtent::DEPTH);
    uint16_t max_height = 0U;
    auto const includeHeight = [&min_height, &max_height](uint16_t const height) noexcept {
        min_height = std::min(min_height, height);
        max_height = std::max(max_height, height);
    };
    for (uint16_t const height : tile.heights) {
        includeHeight(height);
    }
    auto const includeNeighbor = [&includeHeight](std::optional<HeightTile::Heights> const& heights) noexcept {
        if (!heights.has_value()) {
            return;
        }
        for (uint16_t const height : *heights) {
            includeHeight(height);
        }
    };
    includeNeighbor(neighbors.negative_x);
    includeNeighbor(neighbors.positive_x);
    includeNeighbor(neighbors.negative_y);
    includeNeighbor(neighbors.positive_y);
    return boundsForTile(
        player_tile,
        tile.coordinate,
        static_cast<double>(min_height),
        static_cast<double>(max_height)
    );
}

HeightTileSurfaceEdgeProfile heightTileSurfaceEdgeProfile(
    HeightTileSurfaceMesh const& mesh,
    HeightTileSurfaceDirection const edge
)
{
    static_cast<void>(oppositeDirection(edge));
    EdgeSurfaceProfile const profile = edgeSurfaceProfile(mesh, edge);
    int32_t minimum = *std::ranges::min_element(profile.top);
    int32_t maximum = *std::ranges::max_element(profile.top);
    for (auto const& walls : profile.walls) {
        for (auto const& wall : walls) {
            minimum = std::min(minimum, wall.lower);
            maximum = std::max(maximum, wall.upper);
        }
    }
    HeightTileSurfaceEdgeProfile result{
        .elevation = {
            .minimum = static_cast<uint16_t>(minimum),
            .maximum = static_cast<uint16_t>(maximum),
        },
    };
    std::ranges::transform(profile.top, result.top.begin(), [](int32_t const height) {
        return static_cast<uint16_t>(height);
    });
    return result;
}

HeightTileSurfaceElevationRange heightTileSurfaceEdgeElevationRange(
    HeightTileSurfaceMesh const& mesh,
    HeightTileSurfaceDirection const edge
)
{
    return heightTileSurfaceEdgeProfile(mesh, edge).elevation;
}

HeightTileSurfaceFaceAreas heightTileSurfaceFaceAreas(
    HeightTile const& tile,
    HeightTileSurfaceNeighbors const& neighbors,
    std::array<std::optional<HeightTileSurfaceEdgeProfile>, 4> const& installed_neighbor_profiles
)
{
    HeightTileSurfaceNeighbors const compatible_neighbors = neighborsAtSelectedDetail(neighbors);
    HeightTileSurfaceFaceAreas areas;
    static constexpr std::array<HeightTileSurfaceDirection, 4> EDGES{
        HeightTileSurfaceDirection::NegativeX,
        HeightTileSurfaceDirection::PositiveX,
        HeightTileSurfaceDirection::NegativeY,
        HeightTileSurfaceDirection::PositiveY,
    };
    for (uint32_t level = 0U; level < areas.maximum.size(); ++level) {
        uint32_t const cell_size = HeightTileSurfaceFaceAreas::CELL_SIZES[level];
        HeightTile const reduced = reduceTile(tile, cell_size);
        auto const cellMaximumArea = [&](uint32_t const cell_x, uint32_t const cell_y) {
            uint32_t maximum_area = cell_size * cell_size;
            uint16_t const height = reduced.heights[index(cell_x, cell_y)];
            for (uint32_t edge = 0U; edge < EDGES.size(); ++edge) {
                bool const x_edge = edge < 2U;
                bool const negative_edge = edge == 0U || edge == 2U;
                int32_t const perpendicular = static_cast<int32_t>(x_edge ? cell_x : cell_y);
                int32_t const adjacent = negative_edge ? perpendicular - 1
                    : perpendicular + static_cast<int32_t>(cell_size);
                bool const boundary = adjacent < 0 || adjacent >= static_cast<int32_t>(SIDE_LENGTH);
                uint32_t const along_start = x_edge ? cell_y : cell_x;
                auto const adjacentHeight = [&](uint32_t const offset) -> std::optional<uint16_t> {
                    return heightAt(
                        reduced,
                        compatible_neighbors,
                        x_edge ? adjacent : static_cast<int32_t>(offset),
                        x_edge ? static_cast<int32_t>(offset) : adjacent
                    );
                };
                // reduceTile fills each aligned interior cell with one height across its full edge.
                if (boundary) {
                    uint32_t along = along_start;
                    while (along < along_start + cell_size) {
                        std::optional<uint16_t> const adjacent_height = adjacentHeight(along);
                        if (!adjacent_height.has_value()) {
                            ++along;
                            continue;
                        }
                        uint32_t span = 1U;
                        while (along + span < along_start + cell_size
                            && adjacentHeight(along + span) == adjacent_height) {
                            ++span;
                        }
                        if (height > *adjacent_height) {
                            uint32_t const wall_area = span * static_cast<uint32_t>(height - *adjacent_height);
                            maximum_area = std::max(maximum_area, wall_area);
                        }
                        along += span;
                    }
                } else {
                    std::optional<uint16_t> const adjacent_height = adjacentHeight(along_start);
                    if (adjacent_height.has_value() && height > *adjacent_height) {
                        maximum_area = std::max(
                            maximum_area,
                            cell_size * static_cast<uint32_t>(height - *adjacent_height)
                        );
                    }
                }
                if (boundary && installed_neighbor_profiles[edge].has_value()) {
                    auto const& top = installed_neighbor_profiles[edge].value().top;
                    for (uint32_t offset = along_start; offset < along_start + cell_size; ++offset) {
                        uint32_t const gap = height > top[offset] ? height - top[offset] : top[offset] - height;
                        // Seam gaps are contained between the two installed top profiles.
                        maximum_area = std::max(maximum_area, cell_size * gap);
                    }
                }
            }
            return maximum_area;
        };
        uint32_t maximum_area = cell_size * cell_size;
        for (uint32_t cell_y = 0U; cell_y < SIDE_LENGTH; cell_y += cell_size) {
            for (uint32_t cell_x = 0U; cell_x < SIDE_LENGTH; cell_x += cell_size) {
                maximum_area = std::max(maximum_area, cellMaximumArea(cell_x, cell_y));
            }
        }
        areas.maximum[level] = maximum_area;
    }
    return areas;
}

HeightTileSurfaceMesh HeightTileSurfaceMesher::build(
    HeightTile const& tile,
    HeightTileSurfaceNeighbors const& neighbors,
    HeightTileSurfaceDetail const detail
) const
{
    HeightTileSurfaceMesh mesh{
        .coordinate = tile.coordinate,
        .quads = {},
        .detail = detail,
        .identity = tile.surface.identity,
    };
    bool const canonical_surface = hasCanonicalSurface(tile);
    HeightTileSurfaceNeighbors const compatible_neighbors = neighborsAtSelectedDetail(neighbors);
    switch (detail) {
    case HeightTileSurfaceDetail::Fine:
        mesh.quads.reserve(HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT);
        appendTopQuads(mesh, tile, canonical_surface);
        appendWallQuads(mesh, tile, compatible_neighbors, HeightTileSurfaceDirection::NegativeX, canonical_surface);
        appendWallQuads(mesh, tile, compatible_neighbors, HeightTileSurfaceDirection::PositiveX, canonical_surface);
        appendWallQuads(mesh, tile, compatible_neighbors, HeightTileSurfaceDirection::NegativeY, canonical_surface);
        appendWallQuads(mesh, tile, compatible_neighbors, HeightTileSurfaceDirection::PositiveY, canonical_surface);
        break;
    case HeightTileSurfaceDetail::Coarse2:
        mesh.quads.reserve(reducedQuadReservation(COARSE2_CELL_SIZE));
        appendReducedQuads(mesh, tile, compatible_neighbors, COARSE2_CELL_SIZE);
        break;
    case HeightTileSurfaceDetail::Coarse4:
        mesh.quads.reserve(reducedQuadReservation(COARSE4_CELL_SIZE));
        appendReducedQuads(mesh, tile, compatible_neighbors, COARSE4_CELL_SIZE);
        break;
    case HeightTileSurfaceDetail::Coarse:
        mesh.quads.reserve(reducedQuadReservation(COARSE_CELL_SIZE));
        appendReducedQuads(mesh, tile, compatible_neighbors, COARSE_CELL_SIZE);
        break;
    case HeightTileSurfaceDetail::Distant:
        mesh.quads.reserve(reducedQuadReservation(DISTANT_CELL_SIZE));
        appendReducedQuads(mesh, tile, compatible_neighbors, DISTANT_CELL_SIZE);
        break;
    default:
        throw std::invalid_argument{ "height tile mesh has an invalid detail level" };
    }
    mesh.quads.shrink_to_fit();
    return mesh;
}

HeightTileSurfaceSeamBridge heightTileSurfaceSeamBridge(
    HeightTileSurfaceMesh const& first,
    HeightTileSurfaceMesh const& second,
    HeightTileSurfaceDirection const first_edge,
    SurfaceTile const* const first_surface,
    SurfaceTile const* const second_surface
)
{
    HeightTileSurfaceDirection const second_edge = oppositeDirection(first_edge);
    bool const x_edge = first_edge == HeightTileSurfaceDirection::NegativeX
        || first_edge == HeightTileSurfaceDirection::PositiveX;
    uint32_t const first_edge_offset = first_edge == HeightTileSurfaceDirection::NegativeX
            || first_edge == HeightTileSurfaceDirection::NegativeY
        ? 0U
        : static_cast<uint32_t>(HeightTile::SIDE_LENGTH - 1U);
    uint32_t const second_edge_offset = second_edge == HeightTileSurfaceDirection::NegativeX
            || second_edge == HeightTileSurfaceDirection::NegativeY
        ? 0U
        : static_cast<uint32_t>(HeightTile::SIDE_LENGTH - 1U);
    EdgeSurfaceProfile const first_profile = edgeSurfaceProfile(first, first_edge);
    EdgeSurfaceProfile const second_profile = edgeSurfaceProfile(second, second_edge);
    HeightTileSurfaceSeamBridge bridge;
    std::map<std::tuple<bool, int32_t, int32_t, uint8_t>, std::pair<uint32_t, size_t>> spans;
    for (uint32_t along_offset = 0U; along_offset < HeightTile::SIDE_LENGTH; ++along_offset) {
        int32_t const first_height = first_profile.top[along_offset];
        int32_t const second_height = second_profile.top[along_offset];
        if (first_height == second_height) {
            continue;
        }
        bool const first_is_higher = first_height > second_height;
        int32_t const lower = std::min(first_height, second_height);
        int32_t const upper = std::max(first_height, second_height);
        std::vector<VerticalInterval> covered = first_profile.walls[along_offset];
        std::vector<VerticalInterval> const& second_covered = second_profile.walls[along_offset];
        covered.insert(covered.end(), second_covered.begin(), second_covered.end());
        auto& quads = first_is_higher ? bridge.first : bridge.second;
        shared::HeightTileSurfaceMesh const& owner = first_is_higher ? first : second;
        SurfaceTile const* const owner_surface = first_is_higher ? first_surface : second_surface;
        shared::HeightTileSurfaceDirection const owner_edge = first_is_higher ? first_edge : second_edge;
        uint32_t const owner_edge_offset = first_is_higher ? first_edge_offset : second_edge_offset;
        int32_t const owner_height = first_is_higher ? first_height : second_height;
        std::ranges::sort(covered, {}, &VerticalInterval::lower);
        int32_t cursor = lower;
        auto const appendGap = [&](int32_t const gap_lower, int32_t const gap_upper) {
            if (gap_lower >= gap_upper) {
                return;
            }
            int32_t segment_lower = gap_lower;
            while (segment_lower < gap_upper) {
                shared::Block const material = surfaceMaterialAt(
                    owner_surface,
                    owner,
                    owner_edge,
                    along_offset,
                    owner_height,
                    segment_lower
                );
                int32_t segment_upper = segment_lower + 1;
                while (segment_upper < gap_upper
                    && surfaceMaterialAt(owner_surface, owner, owner_edge, along_offset,
                        owner_height, segment_upper) == material) {
                    ++segment_upper;
                }
                auto const key = std::tuple{
                    first_is_higher,
                    segment_lower,
                    segment_upper,
                    static_cast<uint8_t>(material),
                };
                auto const previous = spans.find(key);
                if (previous != spans.end() && previous->second.first + 1U == along_offset) {
                    ++quads[previous->second.second].u_extent;
                    previous->second.first = along_offset;
                } else {
                    uint32_t const local_x = x_edge ? owner_edge_offset : along_offset;
                    uint32_t const local_y = x_edge ? along_offset : owner_edge_offset;
                    quads.push_back({
                        .x = worldCoordinate(owner.coordinate.x, local_x),
                        .y = worldCoordinate(owner.coordinate.y, local_y),
                        .z = segment_lower,
                        .direction = owner_edge,
                        .u_extent = 1U,
                        .v_extent = static_cast<uint16_t>(segment_upper - segment_lower),
                        .material = material,
                    });
                    spans.insert_or_assign(key, std::pair{ along_offset, quads.size() - 1U });
                }
                segment_lower = segment_upper;
            }
        };
        for (VerticalInterval const interval : covered) {
            int32_t const clipped_lower = std::max(lower, interval.lower);
            int32_t const clipped_upper = std::min(upper, interval.upper);
            if (clipped_upper <= cursor) {
                continue;
            }
            if (clipped_lower > cursor) {
                appendGap(cursor, clipped_lower);
            }
            cursor = std::max(cursor, clipped_upper);
            if (cursor >= upper) {
                break;
            }
        }
        appendGap(cursor, upper);
    }
    return bridge;
}

} // namespace shared
