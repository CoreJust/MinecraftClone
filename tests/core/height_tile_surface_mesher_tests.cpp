#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <ranges>

namespace {

[[nodiscard]]
bool hasQuad(
    shared::HeightTileSurfaceMesh const& mesh,
    int32_t const x,
    int32_t const y,
    int32_t const z,
    shared::HeightTileSurfaceDirection const direction,
    uint16_t const u_extent,
    uint16_t const v_extent
)
{
    return std::ranges::any_of(mesh.quads, [=](shared::HeightTileSurfaceQuad const& quad) {
        return quad.x == x && quad.y == y && quad.z == z && quad.direction == direction
            && quad.u_extent == u_extent && quad.v_extent == v_extent;
    });
}

} // namespace

TEST(HeightTileSurfaceMesherTest, FlatTileCollapsesToOneTopAndFourBoundaryQuads)
{
    static constexpr uint16_t HEIGHT = 12U;
    shared::HeightTile tile{ .coordinate = { .x = 3, .y = 5 } };
    tile.heights.fill(HEIGHT);

    shared::HeightTileSurfaceMesh const mesh = shared::HeightTileSurfaceMesher{}.build(tile);

    ASSERT_EQ(mesh.quads.size(), 5U);
    EXPECT_TRUE(hasQuad(
        mesh, 48, 80, HEIGHT - 1, shared::HeightTileSurfaceDirection::PositiveZ, 16U, 16U
    ));
    EXPECT_TRUE(hasQuad(
        mesh, 48, 80, 0, shared::HeightTileSurfaceDirection::NegativeX, 16U, HEIGHT
    ));
    EXPECT_TRUE(hasQuad(
        mesh, 63, 80, 0, shared::HeightTileSurfaceDirection::PositiveX, 16U, HEIGHT
    ));
    EXPECT_TRUE(hasQuad(
        mesh, 48, 80, 0, shared::HeightTileSurfaceDirection::NegativeY, 16U, HEIGHT
    ));
    EXPECT_TRUE(hasQuad(
        mesh, 48, 95, 0, shared::HeightTileSurfaceDirection::PositiveY, 16U, HEIGHT
    ));
}

TEST(HeightTileSurfaceMesherTest, AdjacentColumnsExposeOnlyTheHeightDifference)
{
    shared::HeightTile tile{};
    tile.heights.fill(4U);
    tile.heights[0] = 9U;

    shared::HeightTileSurfaceMesh const mesh = shared::HeightTileSurfaceMesher{}.build(tile);

    EXPECT_TRUE(hasQuad(
        mesh, 0, 0, 4, shared::HeightTileSurfaceDirection::PositiveX, 1U, 5U
    ));
    EXPECT_FALSE(std::ranges::any_of(mesh.quads, [](shared::HeightTileSurfaceQuad const& quad) {
        return quad.direction == shared::HeightTileSurfaceDirection::PositiveX
            && quad.x == 0 && quad.y == 0 && quad.z == 0;
    }));
}

TEST(HeightTileSurfaceMesherTest, SolidNeighborSuppressesSharedBoundaryWhileLowerNeighborExposesSeam)
{
    static constexpr uint16_t HEIGHT = 11U;
    shared::HeightTile tile{};
    tile.heights.fill(HEIGHT);
    shared::HeightTileSurfaceNeighbors neighbors{};
    neighbors.positive_x.emplace();
    neighbors.positive_x->fill(HEIGHT);

    shared::HeightTileSurfaceMesh const seamless = shared::HeightTileSurfaceMesher{}.build(tile, neighbors);
    EXPECT_FALSE(std::ranges::any_of(seamless.quads, [](shared::HeightTileSurfaceQuad const& quad) {
        return quad.direction == shared::HeightTileSurfaceDirection::PositiveX;
    }));

    neighbors.positive_x->fill(6U);
    shared::HeightTileSurfaceMesh const stepped = shared::HeightTileSurfaceMesher{}.build(tile, neighbors);
    EXPECT_TRUE(hasQuad(
        stepped, 15, 0, 6, shared::HeightTileSurfaceDirection::PositiveX, 16U, 5U
    ));
}

TEST(HeightTileSurfaceMesherTest, GeneratedTerrainCoversEveryColumnTop)
{
    shared::TerrainGenerator generator;
    shared::HeightTileSurfaceMesher mesher;
    for (int32_t tile_y = 2'046; tile_y <= 2'050; ++tile_y) {
        for (int32_t tile_x = 2'046; tile_x <= 2'050; ++tile_x) {
            shared::HeightTile const tile = generator.generateHeightTile({.x = tile_x, .y = tile_y});
            shared::HeightTileSurfaceMesh const mesh = mesher.build(tile);
            for (uint32_t y = 0U; y < shared::HeightTile::SIDE_LENGTH; ++y) {
                for (uint32_t x = 0U; x < shared::HeightTile::SIDE_LENGTH; ++x) {
                    int32_t const world_x = tile_x * shared::HeightTile::SIDE_LENGTH + static_cast<int32_t>(x);
                    int32_t const world_y = tile_y * shared::HeightTile::SIDE_LENGTH + static_cast<int32_t>(y);
                    uint16_t const height = tile.heights[y * shared::HeightTile::SIDE_LENGTH + x];
                    EXPECT_TRUE(std::ranges::any_of(mesh.quads, [=](shared::HeightTileSurfaceQuad const& quad) {
                        return quad.direction == shared::HeightTileSurfaceDirection::PositiveZ
                            && quad.z + 1 == height
                            && world_x >= quad.x && world_x < quad.x + quad.u_extent
                            && world_y >= quad.y && world_y < quad.y + quad.v_extent;
                    })) << "missing top at " << world_x << ',' << world_y;
                }
            }
        }
    }
}
