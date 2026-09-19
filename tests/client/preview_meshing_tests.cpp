#include <client/PreviewMeshing.hpp>

#include <gtest/gtest.h>

#include <algorithm>

TEST(PreviewMeshingTest, BuildsFromReceivedHeightsAndNeighborWithoutRegeneratingTerrain)
{
    client::PreviewResidency residency{{.generation = 1U, .revision = 1U}};
    shared::HeightTile::Heights center_heights;
    center_heights.fill(12U);
    shared::HeightTile::Heights western_heights;
    western_heights.fill(20U);
    auto const center = residency.accept({.x = 100, .y = 101}, {1U, 1U}, 1U, center_heights);
    auto const west = residency.accept({.x = 99, .y = 101}, {1U, 1U}, 2U, western_heights);
    ASSERT_EQ(center.replacement, client::HeightTileReplacement::Published);
    ASSERT_EQ(west.replacement, client::HeightTileReplacement::Published);

    shared::HeightTileSurfaceMesh const mesh = client::buildPreviewMesh({
        .tile = center.handle,
        .neighbors = {west.handle, {}, {}, {}},
    });
    EXPECT_EQ(mesh.coordinate, (shared::HeightTileCoordinate{.x = 100, .y = 101}));
    EXPECT_TRUE(std::ranges::any_of(mesh.quads, [](shared::HeightTileSurfaceQuad const& quad) {
        return quad.direction == shared::HeightTileSurfaceDirection::PositiveZ && quad.z == 11;
    }));
    EXPECT_FALSE(std::ranges::any_of(mesh.quads, [](shared::HeightTileSurfaceQuad const& quad) {
        return quad.direction == shared::HeightTileSurfaceDirection::NegativeX;
    }));
}
