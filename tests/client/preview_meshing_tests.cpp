#include <client/PreviewMeshing.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <optional>

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

TEST(PreviewMeshingTest, AppliesTheSelectedVisualDetail)
{
    client::PreviewResidency residency{{.generation = 1U, .revision = 1U}};
    shared::HeightTile::Heights heights;
    heights.fill(12U);
    auto const tile = residency.accept({.x = 100, .y = 101}, {1U, 1U}, 1U, heights);
    ASSERT_EQ(tile.replacement, client::HeightTileReplacement::Published);

    shared::HeightTileSurfaceMesh const mesh = client::buildPreviewMesh({
        .tile = tile.handle,
        .neighbors = {},
        .detail = shared::HeightTileSurfaceDetail::Distant,
    });

    EXPECT_EQ(mesh.detail, shared::HeightTileSurfaceDetail::Distant);
    EXPECT_LT(mesh.quads.size(), shared::HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT);
}

TEST(PreviewMeshingTest, RebuildsWrappedSeamFacesFromTheCurrentNeighborRevision)
{
    client::PreviewResidency residency{{.generation = 1U, .revision = 1U}};
    shared::HeightTile::Heights center_heights;
    center_heights.fill(12U);
    shared::HeightTile::Heights west_heights;
    west_heights.fill(20U);
    auto const center = residency.accept({.x = 0, .y = 101}, {1U, 1U}, 1U, center_heights);
    auto const old_west = residency.accept({.x = 4'095, .y = 101}, {1U, 1U}, 2U, west_heights);
    ASSERT_EQ(center.replacement, client::HeightTileReplacement::Published);
    ASSERT_EQ(old_west.replacement, client::HeightTileReplacement::Published);
    shared::HeightTileSurfaceMesh const old_mesh = client::buildPreviewMesh({
        .tile = center.handle,
        .neighbors = {old_west.handle, {}, {}, {}},
    });
    EXPECT_FALSE(std::ranges::any_of(old_mesh.quads, [](shared::HeightTileSurfaceQuad const& quad) {
        return quad.direction == shared::HeightTileSurfaceDirection::NegativeX;
    }));

    west_heights.fill(4U);
    auto const current_west = residency.accept({.x = 4'095, .y = 101}, {1U, 1U}, 3U, west_heights);
    ASSERT_EQ(current_west.replacement, client::HeightTileReplacement::Published);
    shared::HeightTileSurfaceMesh const current_mesh = client::buildPreviewMesh({
        .tile = center.handle,
        .neighbors = {current_west.handle, {}, {}, {}},
    });

    EXPECT_TRUE(std::ranges::any_of(current_mesh.quads, [](shared::HeightTileSurfaceQuad const& quad) {
        return quad.direction == shared::HeightTileSurfaceDirection::NegativeX
            && quad.z == 4 && quad.v_extent == 8U;
    }));
}

TEST(PreviewMeshingTest, PreservesSeamsBetweenDifferentNeighborDetails)
{
    static constexpr uint16_t BASE_HEIGHT = 15U;
    static constexpr uint16_t EDGE_HEIGHT = 16U;
    client::PreviewResidency residency{{.generation = 1U, .revision = 1U}};
    shared::HeightTile::Heights west_heights;
    west_heights.fill(BASE_HEIGHT);
    west_heights[15U] = EDGE_HEIGHT;
    shared::HeightTile::Heights east_heights;
    east_heights.fill(BASE_HEIGHT);
    east_heights[0U] = EDGE_HEIGHT;
    auto const west = residency.accept({.x = 0, .y = 0}, {1U, 1U}, 1U, west_heights);
    auto const east = residency.accept({.x = 1, .y = 0}, {1U, 1U}, 2U, east_heights);
    ASSERT_EQ(west.replacement, client::HeightTileReplacement::Published);
    ASSERT_EQ(east.replacement, client::HeightTileReplacement::Published);

    shared::HeightTileSurfaceMesh const west_mesh = client::buildPreviewMesh({
        .tile = west.handle,
        .neighbors = {client::HeightTileHandle{}, east.handle, {}, {}},
        .detail = shared::HeightTileSurfaceDetail::Coarse2,
        .neighbor_details = {
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
        },
    });
    shared::HeightTileSurfaceMesh const east_mesh = client::buildPreviewMesh({
        .tile = east.handle,
        .neighbors = {west.handle, {}, {}, {}},
        .detail = shared::HeightTileSurfaceDetail::Fine,
        .neighbor_details = {
            shared::HeightTileSurfaceDetail::Coarse2,
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
        },
    });

    EXPECT_TRUE(std::ranges::any_of(east_mesh.quads, [](shared::HeightTileSurfaceQuad const& quad) {
        return quad.x == 16 && quad.y == 0 && quad.z == BASE_HEIGHT
            && quad.direction == shared::HeightTileSurfaceDirection::NegativeX
            && quad.u_extent == 1U && quad.v_extent == 1U;
    }));
}

TEST(PreviewMeshingTest, PreservesSeamsWhenNeighborMeshesPublishOutOfOrder)
{
    static constexpr uint16_t WEST_HEIGHT = 16U;
    static constexpr uint16_t EAST_HEIGHT = 15U;
    client::PreviewResidency residency{{.generation = 1U, .revision = 1U}};
    shared::HeightTile::Heights west_heights;
    west_heights.fill(WEST_HEIGHT);
    shared::HeightTile::Heights east_heights;
    east_heights.fill(EAST_HEIGHT);
    east_heights[0U] = WEST_HEIGHT;
    auto const west = residency.accept({.x = 0, .y = 0}, {1U, 1U}, 1U, west_heights);
    auto const east = residency.accept({.x = 1, .y = 0}, {1U, 1U}, 2U, east_heights);
    ASSERT_EQ(west.replacement, client::HeightTileReplacement::Published);
    ASSERT_EQ(east.replacement, client::HeightTileReplacement::Published);

    shared::HeightTileSurfaceMesh const old_west_mesh = client::buildPreviewMesh({
        .tile = west.handle,
        .neighbors = {client::HeightTileHandle{}, east.handle, {}, {}},
        .detail = shared::HeightTileSurfaceDetail::Coarse2,
        .neighbor_details = {
            shared::HeightTileSurfaceDetail::Coarse2,
            shared::HeightTileSurfaceDetail::Coarse2,
            shared::HeightTileSurfaceDetail::Coarse2,
            shared::HeightTileSurfaceDetail::Coarse2,
        },
    });
    shared::HeightTileSurfaceMesh const old_east_mesh = client::buildPreviewMesh({
        .tile = east.handle,
        .neighbors = {west.handle, {}, {}, {}},
        .detail = shared::HeightTileSurfaceDetail::Coarse2,
        .neighbor_details = {
            shared::HeightTileSurfaceDetail::Coarse2,
            shared::HeightTileSurfaceDetail::Coarse2,
            shared::HeightTileSurfaceDetail::Coarse2,
            shared::HeightTileSurfaceDetail::Coarse2,
        },
    });
    shared::HeightTileSurfaceMesh const new_west_mesh = client::buildPreviewMesh({
        .tile = west.handle,
        .neighbors = {client::HeightTileHandle{}, east.handle, {}, {}},
        .detail = shared::HeightTileSurfaceDetail::Fine,
        .neighbor_details = {
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
        },
    });
    shared::HeightTileSurfaceMesh const new_east_mesh = client::buildPreviewMesh({
        .tile = east.handle,
        .neighbors = {west.handle, {}, {}, {}},
        .detail = shared::HeightTileSurfaceDetail::Fine,
        .neighbor_details = {
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
        },
    });

    auto const topHeightAt = [](shared::HeightTileSurfaceMesh const& mesh, int32_t const x, int32_t const y)
        -> std::optional<int32_t> {
        for (shared::HeightTileSurfaceQuad const& quad : mesh.quads) {
            if (quad.direction == shared::HeightTileSurfaceDirection::PositiveZ
                && quad.x <= x && x < quad.x + quad.u_extent
                && quad.y <= y && y < quad.y + quad.v_extent) {
                return quad.z + 1;
            }
        }
        return std::nullopt;
    };
    auto const wallCovers = [](shared::HeightTileSurfaceMesh const& mesh,
        shared::HeightTileSurfaceDirection const direction, int32_t const x, int32_t const y,
        int32_t const lower_height, int32_t const upper_height) {
        return std::ranges::any_of(mesh.quads, [=](shared::HeightTileSurfaceQuad const& quad) {
            return quad.direction == direction && quad.x == x
                && quad.y <= y && y < quad.y + quad.u_extent
                && quad.z <= lower_height && upper_height <= quad.z + quad.v_extent;
        });
    };
    auto const seamIsCovered = [&](shared::HeightTileSurfaceMesh const& visible_west,
        shared::HeightTileSurfaceMesh const& visible_east) {
        for (int32_t y = 0; y < static_cast<int32_t>(shared::HeightTile::SIDE_LENGTH); ++y) {
            std::optional<int32_t> const west_height = topHeightAt(visible_west, 15, y);
            std::optional<int32_t> const east_height = topHeightAt(visible_east, 16, y);
            if (!west_height.has_value() || !east_height.has_value() || *west_height == *east_height) {
                if (!west_height.has_value() || !east_height.has_value()) {
                    return false;
                }
                continue;
            }
            int32_t const lower_height = std::min(*west_height, *east_height);
            int32_t const upper_height = std::max(*west_height, *east_height);
            if (!wallCovers(visible_west, shared::HeightTileSurfaceDirection::PositiveX,
                    15, y, lower_height, upper_height)
                && !wallCovers(visible_east, shared::HeightTileSurfaceDirection::NegativeX,
                    16, y, lower_height, upper_height)) {
                return false;
            }
        }
        return true;
    };

    EXPECT_FALSE(seamIsCovered(new_west_mesh, old_east_mesh));
    EXPECT_TRUE(seamIsCovered(old_west_mesh, old_east_mesh));

    // The west result arrives first; recompute both visible variants before draw.
    client::PreviewMeshSeamBridgeSet west_bridges;
    client::PreviewMeshSeamBridgeSet east_bridges;
    std::array<client::PreviewMeshSeamNeighbor, client::MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT>
        visible_east_neighbors{};
    visible_east_neighbors[1] = {
        .mesh = &old_east_mesh,
        .bridges = &east_bridges,
    };
    client::PreviewMeshSeamNeighborUpdates const west_first_updates =
        client::updatePreviewMeshSeamBridgesForVisibleNeighbors(
        new_west_mesh,
        west_bridges,
        visible_east_neighbors
    );
    EXPECT_LE(west_first_updates.count, client::MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT);
    shared::HeightTileSurfaceMesh const bridged_new_west = west_bridges.compose(new_west_mesh);
    shared::HeightTileSurfaceMesh const bridged_old_east = east_bridges.compose(old_east_mesh);
    EXPECT_TRUE(seamIsCovered(bridged_new_west, bridged_old_east));

    visible_east_neighbors[1].mesh = &new_east_mesh;
    static_cast<void>(client::updatePreviewMeshSeamBridgesForVisibleNeighbors(
        new_west_mesh,
        west_bridges,
        visible_east_neighbors
    ));
    shared::HeightTileSurfaceMesh const completed_west = west_bridges.compose(new_west_mesh);
    shared::HeightTileSurfaceMesh const completed_east = east_bridges.compose(new_east_mesh);
    EXPECT_TRUE(seamIsCovered(completed_west, completed_east));

    // The east result may arrive first; the opposite transition order is equivalent.
    client::PreviewMeshSeamBridgeSet reverse_east_bridges;
    client::PreviewMeshSeamBridgeSet reverse_west_bridges;
    std::array<client::PreviewMeshSeamNeighbor, client::MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT>
        visible_west_neighbors{};
    visible_west_neighbors[0] = {
        .mesh = &old_west_mesh,
        .bridges = &reverse_west_bridges,
    };
    client::PreviewMeshSeamNeighborUpdates const east_first_updates =
        client::updatePreviewMeshSeamBridgesForVisibleNeighbors(
        new_east_mesh,
        reverse_east_bridges,
        visible_west_neighbors
    );
    EXPECT_LE(east_first_updates.count, client::MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT);
    shared::HeightTileSurfaceMesh const bridged_new_east = reverse_east_bridges.compose(new_east_mesh);
    shared::HeightTileSurfaceMesh const bridged_old_west = reverse_west_bridges.compose(old_west_mesh);
    EXPECT_TRUE(seamIsCovered(bridged_old_west, bridged_new_east));
    EXPECT_TRUE(seamIsCovered(old_west_mesh, old_east_mesh));
}

TEST(PreviewMeshingTest, BuildsTransitionBridgeAtNormalizedWorldWrapBoundary)
{
    client::PreviewResidency residency{{.generation = 1U, .revision = 1U}};
    shared::HeightTile::Heights west_heights;
    west_heights.fill(16U);
    shared::HeightTile::Heights east_heights;
    east_heights.fill(15U);
    east_heights[0U] = 16U;
    auto const west = residency.accept({.x = 4'095, .y = 0}, {1U, 1U}, 1U, west_heights);
    auto const east = residency.accept({.x = 0, .y = 0}, {1U, 1U}, 2U, east_heights);
    ASSERT_EQ(west.replacement, client::HeightTileReplacement::Published);
    ASSERT_EQ(east.replacement, client::HeightTileReplacement::Published);

    shared::HeightTileSurfaceMesh const new_west = client::buildPreviewMesh({
        .tile = west.handle,
        .neighbors = {client::HeightTileHandle{}, east.handle, {}, {}},
        .detail = shared::HeightTileSurfaceDetail::Fine,
        .neighbor_details = {
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
        },
    });
    shared::HeightTileSurfaceMesh const old_east = client::buildPreviewMesh({
        .tile = east.handle,
        .neighbors = {west.handle, {}, {}, {}},
        .detail = shared::HeightTileSurfaceDetail::Coarse2,
        .neighbor_details = {
            shared::HeightTileSurfaceDetail::Coarse2,
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
        },
    });

    client::PreviewMeshSeamBridgeSet west_bridges;
    client::PreviewMeshSeamBridgeSet east_bridges;
    std::array<client::PreviewMeshSeamNeighbor, client::MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT>
        visible_neighbors{};
    visible_neighbors[1] = {
        .mesh = &old_east,
        .bridges = &east_bridges,
    };
    static_cast<void>(client::updatePreviewMeshSeamBridgesForVisibleNeighbors(
        new_west,
        west_bridges,
        visible_neighbors
    ));
    ASSERT_FALSE(west_bridges.edges()[1].empty());
    EXPECT_TRUE(std::ranges::all_of(west_bridges.edges()[1], [](shared::HeightTileSurfaceQuad const& quad) {
        return quad.x == 65'535
            && quad.direction == shared::HeightTileSurfaceDirection::PositiveX;
    }));
}

TEST(PreviewMeshingTest, LimitsOneResultToFourNeighborBridgeUpdates)
{
    auto const flatMesh = [](shared::HeightTileCoordinate coordinate, uint16_t const height) {
        int32_t const x = coordinate.x * static_cast<int32_t>(shared::HeightTile::SIDE_LENGTH);
        int32_t const y = coordinate.y * static_cast<int32_t>(shared::HeightTile::SIDE_LENGTH);
        return shared::HeightTileSurfaceMesh{
            .coordinate = coordinate,
            .quads = {{
                .x = x,
                .y = y,
                .z = static_cast<int32_t>(height) - 1,
                .direction = shared::HeightTileSurfaceDirection::PositiveZ,
                .u_extent = shared::HeightTile::SIDE_LENGTH,
                .v_extent = shared::HeightTile::SIDE_LENGTH,
            }},
            .detail = shared::HeightTileSurfaceDetail::Fine,
        };
    };
    shared::HeightTileSurfaceMesh const low_mesh = flatMesh({.x = 10, .y = 10}, 15U);
    std::array<shared::HeightTileSurfaceMesh, client::MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT>
        high_meshes{
            flatMesh({.x = 9, .y = 10}, 16U),
            flatMesh({.x = 11, .y = 10}, 16U),
            flatMesh({.x = 10, .y = 9}, 16U),
            flatMesh({.x = 10, .y = 11}, 16U),
        };
    std::array<client::PreviewMeshSeamBridgeSet, client::MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT>
        neighbor_bridges;
    std::array<client::PreviewMeshSeamNeighbor, client::MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT>
        neighbors{};
    for (size_t index = 0U; index < neighbors.size(); ++index) {
        neighbors[index] = {
            .mesh = &high_meshes[index],
            .bridges = &neighbor_bridges[index],
        };
    }
    client::PreviewMeshSeamBridgeSet low_bridges;
    client::PreviewMeshSeamNeighborUpdates const updates =
        client::updatePreviewMeshSeamBridgesForVisibleNeighbors(low_mesh, low_bridges, neighbors);

    EXPECT_EQ(updates.count, client::MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT);
    EXPECT_LE(updates.count, 4U);
}
