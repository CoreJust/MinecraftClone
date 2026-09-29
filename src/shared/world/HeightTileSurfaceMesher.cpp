#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <shared/world/SparseWorld.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <tuple>

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

[[nodiscard]]
constexpr int32_t worldCoordinate(int32_t const tile_coordinate, uint32_t const local_coordinate) noexcept
{
    return tile_coordinate * static_cast<int32_t>(SIDE_LENGTH) + static_cast<int32_t>(local_coordinate);
}

[[nodiscard]]
uint16_t heightAt(
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
    return 0U;
}

void appendTopQuads(shared::HeightTileSurfaceMesh& mesh, shared::HeightTile const& tile)
{
    std::array<bool, shared::HeightTile::SAMPLE_COUNT> consumed{};
    for (uint32_t y = 0U; y < SIDE_LENGTH; ++y) {
        for (uint32_t x = 0U; x < SIDE_LENGTH; ++x) {
            uint32_t const start = index(x, y);
            uint16_t const height = tile.heights[start];
            if (height == 0U || consumed[start]) {
                continue;
            }
            uint32_t width = 1U;
            while (x + width < SIDE_LENGTH && !consumed[index(x + width, y)]
                && tile.heights[index(x + width, y)] == height) {
                ++width;
            }
            uint32_t depth = 1U;
            bool matches = true;
            while (y + depth < SIDE_LENGTH && matches) {
                for (uint32_t column = 0U; column < width; ++column) {
                    uint32_t const candidate = index(x + column, y + depth);
                    if (consumed[candidate] || tile.heights[candidate] != height) {
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

void appendXQuads(
    shared::HeightTileSurfaceMesh& mesh,
    shared::HeightTile const& tile,
    shared::HeightTileSurfaceNeighbors const& neighbors,
    shared::HeightTileSurfaceDirection const direction
)
{
    int32_t const offset = direction == shared::HeightTileSurfaceDirection::NegativeX ? -1 : 1;
    for (uint32_t x = 0U; x < SIDE_LENGTH; ++x) {
        uint32_t y = 0U;
        while (y < SIDE_LENGTH) {
            uint16_t const height = tile.heights[index(x, y)];
            uint16_t const adjacent = heightAt(tile, neighbors, static_cast<int32_t>(x) + offset, static_cast<int32_t>(y));
            if (height <= adjacent) {
                ++y;
                continue;
            }
            uint32_t span = 1U;
            while (y + span < SIDE_LENGTH) {
                uint16_t const next_height = tile.heights[index(x, y + span)];
                uint16_t const next_adjacent = heightAt(
                    tile,
                    neighbors,
                    static_cast<int32_t>(x) + offset,
                    static_cast<int32_t>(y + span)
                );
                if (next_height != height || next_adjacent != adjacent) {
                    break;
                }
                ++span;
            }
            mesh.quads.push_back({
                .x = worldCoordinate(tile.coordinate.x, x),
                .y = worldCoordinate(tile.coordinate.y, y),
                .z = static_cast<int32_t>(adjacent),
                .direction = direction,
                .u_extent = static_cast<uint16_t>(span),
                .v_extent = static_cast<uint16_t>(height - adjacent),
            });
            y += span;
        }
    }
}

void appendYQuads(
    shared::HeightTileSurfaceMesh& mesh,
    shared::HeightTile const& tile,
    shared::HeightTileSurfaceNeighbors const& neighbors,
    shared::HeightTileSurfaceDirection const direction
)
{
    int32_t const offset = direction == shared::HeightTileSurfaceDirection::NegativeY ? -1 : 1;
    for (uint32_t y = 0U; y < SIDE_LENGTH; ++y) {
        uint32_t x = 0U;
        while (x < SIDE_LENGTH) {
            uint16_t const height = tile.heights[index(x, y)];
            uint16_t const adjacent = heightAt(tile, neighbors, static_cast<int32_t>(x), static_cast<int32_t>(y) + offset);
            if (height <= adjacent) {
                ++x;
                continue;
            }
            uint32_t span = 1U;
            while (x + span < SIDE_LENGTH) {
                uint16_t const next_height = tile.heights[index(x + span, y)];
                uint16_t const next_adjacent = heightAt(
                    tile,
                    neighbors,
                    static_cast<int32_t>(x + span),
                    static_cast<int32_t>(y) + offset
                );
                if (next_height != height || next_adjacent != adjacent) {
                    break;
                }
                ++span;
            }
            mesh.quads.push_back({
                .x = worldCoordinate(tile.coordinate.x, x),
                .y = worldCoordinate(tile.coordinate.y, y),
                .z = static_cast<int32_t>(adjacent),
                .direction = direction,
                .u_extent = static_cast<uint16_t>(span),
                .v_extent = static_cast<uint16_t>(height - adjacent),
            });
            x += span;
        }
    }
}

shared::HeightTile reduceTile(shared::HeightTile const& tile, uint32_t const cell_size)
{
    shared::HeightTile coarse_tile{
        .coordinate = tile.coordinate,
        .heights = {},
    };
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
            for (uint32_t y = coarse_y; y < coarse_y + cell_size; ++y) {
                for (uint32_t x = coarse_x; x < coarse_x + cell_size; ++x) {
                    coarse_tile.heights[index(x, y)] = height;
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
    appendTopQuads(mesh, coarse_tile);
    appendXQuads(mesh, coarse_tile, neighbors, shared::HeightTileSurfaceDirection::NegativeX);
    appendXQuads(mesh, coarse_tile, neighbors, shared::HeightTileSurfaceDirection::PositiveX);
    appendYQuads(mesh, coarse_tile, neighbors, shared::HeightTileSurfaceDirection::NegativeY);
    appendYQuads(mesh, coarse_tile, neighbors, shared::HeightTileSurfaceDirection::PositiveY);
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
    EdgeSurfaceProfile profile;
    for (shared::HeightTileSurfaceQuad const& quad : mesh.quads) {
        if (quad.direction == shared::HeightTileSurfaceDirection::PositiveZ) {
            for (uint32_t along = 0U; along < shared::HeightTile::SIDE_LENGTH; ++along) {
                uint32_t const local_x = x_edge ? edge_offset : along;
                uint32_t const local_y = x_edge ? along : edge_offset;
                int32_t const x = worldCoordinate(mesh.coordinate.x, local_x);
                int32_t const y = worldCoordinate(mesh.coordinate.y, local_y);
                if (quad.x <= x && x < quad.x + quad.u_extent
                    && quad.y <= y && y < quad.y + quad.v_extent) {
                    profile.top[along] = quad.z + 1;
                }
            }
        } else if (quad.direction == edge) {
            for (uint32_t along = 0U; along < shared::HeightTile::SIDE_LENGTH; ++along) {
                uint32_t const local_x = x_edge ? edge_offset : along;
                uint32_t const local_y = x_edge ? along : edge_offset;
                int32_t const x = worldCoordinate(mesh.coordinate.x, local_x);
                int32_t const y = worldCoordinate(mesh.coordinate.y, local_y);
                bool const covers_sample = x_edge
                    ? quad.x == x && quad.y <= y && y < quad.y + quad.u_extent
                    : quad.y == y && quad.x <= x && x < quad.x + quad.u_extent;
                if (covers_sample) {
                    profile.walls[along].push_back({ quad.z, quad.z + quad.v_extent });
                }
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
    // This conservative corner bound sets the projected-area limit for a source face.
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

    auto const axisSeparation = [](double const first_min, double const first_max,
        double const second_min, double const second_max) noexcept {
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
    double const surface_height = surface_bounds.max_z_blocks - surface_bounds.min_z_blocks;
    auto const cutoffDistance = [this, surface_height](double const cell_size) noexcept {
        double const face_area = std::max(
            cell_size * cell_size,
            cell_size * surface_height
        );
        return m_first_reduction_distance * std::sqrt(face_area);
    };
    HeightTileSurfaceDetail const selected_detail = distance < cutoffDistance(COARSE2_CELL_SIZE)
        ? HeightTileSurfaceDetail::Fine
        : distance < cutoffDistance(COARSE4_CELL_SIZE)
            ? HeightTileSurfaceDetail::Coarse2
            : distance < cutoffDistance(COARSE_CELL_SIZE)
                ? HeightTileSurfaceDetail::Coarse4
                : distance < cutoffDistance(DISTANT_CELL_SIZE)
                    ? HeightTileSurfaceDetail::Coarse
                    : HeightTileSurfaceDetail::Distant;
    if (!current_detail.has_value()) {
        return selected_detail;
    }
    auto const cellSize = [](HeightTileSurfaceDetail const detail) noexcept {
        switch (detail) {
        case HeightTileSurfaceDetail::Coarse:
            return COARSE_CELL_SIZE;
        case HeightTileSurfaceDetail::Coarse2:
            return COARSE2_CELL_SIZE;
        case HeightTileSurfaceDetail::Coarse4:
            return COARSE4_CELL_SIZE;
        case HeightTileSurfaceDetail::Fine:
            return 1U;
        case HeightTileSurfaceDetail::Distant:
            return DISTANT_CELL_SIZE;
        }
        return 1U;
    };
    uint32_t const current_cell_size = cellSize(*current_detail);
    if (cellSize(selected_detail) <= current_cell_size) {
        return selected_detail;
    }
    uint32_t const next_cell_size = current_cell_size * 2U;
    if (distance < cutoffDistance(static_cast<double>(next_cell_size))
            * COARSENING_HYSTERESIS_RATIO) {
        return *current_detail;
    }
    return selected_detail;
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
    for (uint32_t offset = 0U; offset < SIDE_LENGTH; ++offset) {
        uint32_t const local_offset = static_cast<uint32_t>(offset);
        if (neighbors.negative_x.has_value()) {
            includeHeight((*neighbors.negative_x)[index(SIDE_LENGTH - 1U, local_offset)]);
        } else {
            includeHeight(0U);
        }
        if (neighbors.positive_x.has_value()) {
            includeHeight((*neighbors.positive_x)[index(0U, local_offset)]);
        } else {
            includeHeight(0U);
        }
        if (neighbors.negative_y.has_value()) {
            includeHeight((*neighbors.negative_y)[index(local_offset, SIDE_LENGTH - 1U)]);
        } else {
            includeHeight(0U);
        }
        if (neighbors.positive_y.has_value()) {
            includeHeight((*neighbors.positive_y)[index(local_offset, 0U)]);
        } else {
            includeHeight(0U);
        }
    }
    return boundsForTile(
        player_tile,
        tile.coordinate,
        static_cast<double>(min_height),
        static_cast<double>(max_height)
    );
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
    };
    HeightTileSurfaceNeighbors const compatible_neighbors = neighborsAtSelectedDetail(neighbors);
    switch (detail) {
    case HeightTileSurfaceDetail::Fine:
        mesh.quads.reserve(HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT);
        appendTopQuads(mesh, tile);
        appendXQuads(mesh, tile, compatible_neighbors, HeightTileSurfaceDirection::NegativeX);
        appendXQuads(mesh, tile, compatible_neighbors, HeightTileSurfaceDirection::PositiveX);
        appendYQuads(mesh, tile, compatible_neighbors, HeightTileSurfaceDirection::NegativeY);
        appendYQuads(mesh, tile, compatible_neighbors, HeightTileSurfaceDirection::PositiveY);
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
    HeightTileSurfaceDirection const first_edge
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
    std::map<std::tuple<bool, int32_t, int32_t>, std::pair<uint32_t, size_t>> spans;
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
        shared::HeightTileSurfaceDirection const owner_edge = first_is_higher ? first_edge : second_edge;
        uint32_t const owner_edge_offset = first_is_higher ? first_edge_offset : second_edge_offset;
        std::ranges::sort(covered, {}, &VerticalInterval::lower);
        int32_t cursor = lower;
        auto const appendGap = [&](int32_t const gap_lower, int32_t const gap_upper) {
            if (gap_lower >= gap_upper) {
                return;
            }
            auto const key = std::tuple{ first_is_higher, gap_lower, gap_upper };
            auto const previous = spans.find(key);
            if (previous != spans.end() && previous->second.first + 1U == along_offset) {
                ++quads[previous->second.second].u_extent;
                previous->second.first = along_offset;
                return;
            }
            uint32_t const local_x = x_edge ? owner_edge_offset : along_offset;
            uint32_t const local_y = x_edge ? along_offset : owner_edge_offset;
            quads.push_back({
                .x = worldCoordinate(owner.coordinate.x, local_x),
                .y = worldCoordinate(owner.coordinate.y, local_y),
                .z = gap_lower,
                .direction = owner_edge,
                .u_extent = 1U,
                .v_extent = static_cast<uint16_t>(gap_upper - gap_lower),
            });
            spans.insert_or_assign(key, std::pair{ along_offset, quads.size() - 1U });
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
