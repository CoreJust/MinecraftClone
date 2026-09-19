#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <array>
#include <cstdint>

namespace {

constexpr uint32_t SIDE_LENGTH = shared::HeightTile::SIDE_LENGTH;

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

} // namespace

namespace shared {

HeightTileSurfaceMesh HeightTileSurfaceMesher::build(
    HeightTile const& tile,
    HeightTileSurfaceNeighbors const& neighbors
) const
{
    HeightTileSurfaceMesh mesh{ .coordinate = tile.coordinate, .quads = {} };
    mesh.quads.reserve(HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT);
    appendTopQuads(mesh, tile);
    appendXQuads(mesh, tile, neighbors, HeightTileSurfaceDirection::NegativeX);
    appendXQuads(mesh, tile, neighbors, HeightTileSurfaceDirection::PositiveX);
    appendYQuads(mesh, tile, neighbors, HeightTileSurfaceDirection::NegativeY);
    appendYQuads(mesh, tile, neighbors, HeightTileSurfaceDirection::PositiveY);
    mesh.quads.shrink_to_fit();
    return mesh;
}

} // namespace shared
