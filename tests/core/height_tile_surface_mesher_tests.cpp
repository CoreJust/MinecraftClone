#include <shared/world/HeightTileSurfaceMesher.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

template <typename Value>
Value const& requiredValue(std::optional<Value> const& optional)
{
    if (optional.has_value()) {
        return *optional;
    }
    throw std::runtime_error("Test expected an optional value");
}

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

[[nodiscard]]
std::optional<int32_t> topHeightAt(
    shared::HeightTileSurfaceMesh const& mesh,
    int32_t const x,
    int32_t const y
)
{
    auto const top = std::ranges::find_if(mesh.quads, [=](shared::HeightTileSurfaceQuad const& quad) {
        return quad.direction == shared::HeightTileSurfaceDirection::PositiveZ
            && x >= quad.x && x < quad.x + quad.u_extent
            && y >= quad.y && y < quad.y + quad.v_extent;
    });
    if (top == mesh.quads.end()) {
        return std::nullopt;
    }
    return top->z + 1;
}

[[nodiscard]]
std::vector<shared::HeightTileSurfaceQuad> cellPatches(
    std::vector<shared::HeightTileSurfaceQuad> const& quads,
    uint32_t const cell_size
)
{
    std::vector<shared::HeightTileSurfaceQuad> patches;
    auto const remainingCellSpan = [cell_size](int32_t const coordinate) {
        int32_t const size = static_cast<int32_t>(cell_size);
        return cell_size - static_cast<uint32_t>((coordinate % size + size) % size);
    };
    for (shared::HeightTileSurfaceQuad const& quad : quads) {
        bool const top = quad.direction == shared::HeightTileSurfaceDirection::PositiveZ;
        bool const x_wall = quad.direction == shared::HeightTileSurfaceDirection::NegativeX
            || quad.direction == shared::HeightTileSurfaceDirection::PositiveX;
        int32_t const along = x_wall ? quad.y : quad.x;
        for (uint32_t u = 0U; u < quad.u_extent;) {
            uint32_t const width = std::min(static_cast<uint32_t>(quad.u_extent) - u,
                remainingCellSpan(along + static_cast<int32_t>(u)));
            for (uint32_t v = 0U; v < quad.v_extent;) {
                uint32_t const depth = top
                    ? std::min(static_cast<uint32_t>(quad.v_extent) - v,
                        remainingCellSpan(quad.y + static_cast<int32_t>(v)))
                    : static_cast<uint32_t>(quad.v_extent);
                shared::HeightTileSurfaceQuad patch = quad;
                patch.x += top || !x_wall ? static_cast<int32_t>(u) : 0;
                patch.y += top ? static_cast<int32_t>(v) : x_wall ? static_cast<int32_t>(u) : 0;
                patch.u_extent = static_cast<uint16_t>(width);
                patch.v_extent = static_cast<uint16_t>(depth);
                patches.push_back(patch);
                v += depth;
            }
            u += width;
        }
    }
    return patches;
}

void expectPatchAreas(
    std::vector<shared::HeightTileSurfaceQuad> const& quads,
    uint32_t const cell_size,
    double const maximum_area
)
{
    for (shared::HeightTileSurfaceQuad const& patch : cellPatches(quads, cell_size)) {
        EXPECT_LE(static_cast<uint32_t>(patch.u_extent) * patch.v_extent, maximum_area);
    }
}

[[nodiscard]]
double projectedPatchArea(
    shared::HeightTileSurfaceQuad const& patch,
    std::array<double, 3> const eye,
    shared::HeightTileSurfaceProjection const projection
)
{
    static constexpr double DEGREES_TO_RADIANS = 0.017'453'292'519'943'295'769;
    std::array<double, 3> forward{ 8.0 - eye[0], 8.0 - eye[1], 8.0 - eye[2] };
    double const length = std::hypot(forward[0], forward[1], forward[2]);
    for (double& component : forward) {
        component /= length;
    }
    double const horizontal_length = std::hypot(forward[0], forward[1]);
    std::array<double, 3> const right{ -forward[1] / horizontal_length,
        forward[0] / horizontal_length, 0.0 };
    std::array<double, 3> const up{
        -forward[2] * right[1], forward[2] * right[0], horizontal_length,
    };
    double const focal = static_cast<double>(projection.viewport_height_pixels)
        / (2.0 * std::tan(projection.vertical_fov_degrees * DEGREES_TO_RADIANS / 2.0));
    bool const top = patch.direction == shared::HeightTileSurfaceDirection::PositiveZ;
    bool const x_wall = patch.direction == shared::HeightTileSurfaceDirection::NegativeX
        || patch.direction == shared::HeightTileSurfaceDirection::PositiveX;
    std::array<std::array<double, 2>, 4> pixels{};
    static constexpr std::array<std::array<uint32_t, 2>, 4> CORNERS{
        std::array<uint32_t, 2>{ 0U, 0U },
        std::array<uint32_t, 2>{ 1U, 0U },
        std::array<uint32_t, 2>{ 1U, 1U },
        std::array<uint32_t, 2>{ 0U, 1U },
    };
    for (uint32_t corner = 0U; corner < CORNERS.size(); ++corner) {
        double const u = CORNERS[corner][0] * patch.u_extent;
        double const v = CORNERS[corner][1] * patch.v_extent;
        std::array<double, 3> point{ static_cast<double>(patch.x), static_cast<double>(patch.y),
            static_cast<double>(patch.z) };
        if (top) {
            point[0] += u;
            point[1] += v;
            point[2] += 1.0;
        } else if (x_wall) {
            point[0] += patch.direction == shared::HeightTileSurfaceDirection::PositiveX ? 1.0 : 0.0;
            point[1] += u;
            point[2] += v;
        } else {
            point[0] += u;
            point[1] += patch.direction == shared::HeightTileSurfaceDirection::PositiveY ? 1.0 : 0.0;
            point[2] += v;
        }
        double depth = 0.0;
        double horizontal = 0.0;
        double vertical = 0.0;
        for (uint32_t axis = 0U; axis < point.size(); ++axis) {
            double const relative = point[axis] - eye[axis];
            depth += relative * forward[axis];
            horizontal += relative * right[axis];
            vertical += relative * up[axis];
        }
        EXPECT_GT(depth, 0.0);
        pixels[corner] = { focal * horizontal / depth, focal * vertical / depth };
        EXPECT_LE(std::abs(pixels[corner][0]), projection.viewport_width_pixels / 2.0);
        EXPECT_LE(std::abs(pixels[corner][1]), projection.viewport_height_pixels / 2.0);
    }
    double twice_area = 0.0;
    for (uint32_t corner = 0U; corner < pixels.size(); ++corner) {
        auto const& next = pixels[(corner + 1U) % static_cast<uint32_t>(pixels.size())];
        twice_area += pixels[corner][0] * next[1] - next[0] * pixels[corner][1];
    }
    return std::abs(twice_area) / 2.0;
}

} // namespace

TEST(HeightTileSurfaceMesherTest, ReplacementCellCertificateUsesLocalWallsInsteadOfWholeHeightSpan)
{
    static constexpr uint32_t SIDE = shared::HeightTile::SIDE_LENGTH;
    static constexpr shared::HeightTileSurfaceProjection PROJECTION{ 70.0, 2'560U, 1'440U };
    static constexpr shared::HeightTileSurfaceBounds VIEWER{};
    shared::HeightTile tile{ .coordinate = { .x = 240, .y = 0 } };
    for (uint32_t y = 0U; y < SIDE; ++y) {
        for (uint32_t x = 0U; x < SIDE; ++x) {
            tile.heights[y * SIDE + x] = static_cast<uint16_t>(8U + x / 2U);
        }
    }
    shared::HeightTile::Heights low;
    shared::HeightTile::Heights high;
    low.fill(8U);
    high.fill(15U);
    shared::HeightTileSurfaceNeighbors const neighbors{
        .negative_x = low,
        .positive_x = high,
        .negative_y = tile.heights,
        .positive_y = tile.heights,
    };
    auto const areas = shared::heightTileSurfaceFaceAreas(tile, neighbors);
    auto const bounds = shared::HeightTileSurfaceMesher::boundsForTile({}, tile, neighbors);
    shared::HeightTileSurfaceLodPolicy const policy{ PROJECTION };
    EXPECT_EQ(areas.maximum[0], 4.0);
    EXPECT_EQ(policy.detailFor(VIEWER, bounds), shared::HeightTileSurfaceDetail::Fine);
    EXPECT_EQ(policy.detailFor(VIEWER, bounds, areas), shared::HeightTileSurfaceDetail::Coarse2);
}

TEST(HeightTileSurfaceMesherTest, CertificateBoundsPublicMeshAndBothInstalledSeamOwners)
{
    static constexpr uint32_t CASES = 8U;
    static constexpr int32_t WORLD_TILES = 4'096;
    static constexpr std::array<shared::HeightTileSurfaceDirection, 4> EDGES{
        shared::HeightTileSurfaceDirection::NegativeX,
        shared::HeightTileSurfaceDirection::PositiveX,
        shared::HeightTileSurfaceDirection::NegativeY,
        shared::HeightTileSurfaceDirection::PositiveY,
    };
    static constexpr std::array<shared::HeightTileCoordinate, 4> NEIGHBOR_COORDINATES{
        shared::HeightTileCoordinate{ -1, 0 },
        shared::HeightTileCoordinate{ 1, 0 },
        shared::HeightTileCoordinate{ 0, -1 },
        shared::HeightTileCoordinate{ 0, 1 },
    };
    static constexpr std::array<uint32_t, 4> OPPOSITE{ 1U, 0U, 3U, 2U };
    shared::HeightTileSurfaceMesher const mesher;
    uint32_t seed = 42U;
    bool saw_first_bridge = false;
    bool saw_second_bridge = false;
    for (uint32_t case_index = 0U; case_index < CASES; ++case_index) {
        shared::HeightTile tile{
            .coordinate = case_index == 1U ? shared::HeightTileCoordinate{ WORLD_TILES - 1, WORLD_TILES - 1 }
                : shared::HeightTileCoordinate{},
        };
        std::array<shared::HeightTile, 4> raw_neighbors;
        std::array<shared::HeightTileSurfaceMesh, 4> installed;
        std::array<std::optional<shared::HeightTileSurfaceEdgeProfile>, 4> profiles;
        auto const fill = [&seed, case_index](shared::HeightTile& target) {
            for (uint16_t& height : target.heights) {
                seed = seed * 1'664'525U + 1'013'904'223U;
                height = static_cast<uint16_t>(case_index < 4U ? seed % 25U : seed % 1'025U);
            }
        };
        fill(tile);
        for (uint32_t edge = 0U; edge < EDGES.size(); ++edge) {
            raw_neighbors[edge].coordinate = {
                (tile.coordinate.x + NEIGHBOR_COORDINATES[edge].x + WORLD_TILES) % WORLD_TILES,
                (tile.coordinate.y + NEIGHBOR_COORDINATES[edge].y + WORLD_TILES) % WORLD_TILES,
            };
            fill(raw_neighbors[edge]);
            shared::HeightTile retained = raw_neighbors[edge];
            fill(retained);
            shared::HeightTile::Heights taller;
            taller.fill(1'024U);
            installed[edge] = mesher.build(retained, {
                .negative_x = taller, .positive_x = taller,
                .negative_y = taller, .positive_y = taller,
            },
                shared::HeightTileSurfaceFaceAreas::DETAILS[(case_index + edge) % 4U]);
            profiles[edge] = shared::heightTileSurfaceEdgeProfile(installed[edge], EDGES[OPPOSITE[edge]]);
        }
        shared::HeightTileSurfaceNeighbors const neighbors{
            .negative_x = raw_neighbors[0].heights,
            .positive_x = raw_neighbors[1].heights,
            .negative_y = case_index == 0U ? std::nullopt : std::optional{raw_neighbors[2].heights},
            .positive_y = raw_neighbors[3].heights,
            .negative_x_detail = shared::HeightTileSurfaceDetail::Fine,
            .positive_x_detail = shared::HeightTileSurfaceDetail::Coarse2,
            .negative_y_detail = shared::HeightTileSurfaceDetail::Coarse,
            .positive_y_detail = shared::HeightTileSurfaceDetail::Distant,
        };
        auto const areas = shared::heightTileSurfaceFaceAreas(tile, neighbors, profiles);
        for (uint32_t level = 0U; level < areas.maximum.size(); ++level) {
            auto const mesh = mesher.build(tile, neighbors, shared::HeightTileSurfaceFaceAreas::DETAILS[level]);
            uint32_t const cell_size = shared::HeightTileSurfaceFaceAreas::CELL_SIZES[level];
            expectPatchAreas(mesh.quads, cell_size, areas.maximum[level]);
            for (uint32_t edge = 0U; edge < EDGES.size(); ++edge) {
                auto const bridge = shared::heightTileSurfaceSeamBridge(mesh, installed[edge], EDGES[edge]);
                saw_first_bridge = saw_first_bridge || !bridge.first.empty();
                saw_second_bridge = saw_second_bridge || !bridge.second.empty();
                expectPatchAreas(bridge.first, cell_size, areas.maximum[level]);
                expectPatchAreas(bridge.second, cell_size, areas.maximum[level]);
            }
        }
    }
    EXPECT_TRUE(saw_first_bridge);
    EXPECT_TRUE(saw_second_bridge);
}

TEST(HeightTileSurfaceMesherTest, InstalledEdgeTopShapeMattersWhenElevationRangesMatch)
{
    static constexpr uint32_t SIDE = shared::HeightTile::SIDE_LENGTH;
    shared::HeightTile tile;
    for (uint32_t y = 0U; y < SIDE; ++y) {
        for (uint32_t x = 0U; x < SIDE; ++x) {
            tile.heights[y * SIDE + x] = static_cast<uint16_t>(y + 1U);
        }
    }
    shared::HeightTile::Heights low;
    shared::HeightTile::Heights high;
    low.fill(1U);
    high.fill(SIDE);
    shared::HeightTileSurfaceNeighbors const neighbors{
        .negative_x = tile.heights,
        .positive_x = tile.heights,
        .negative_y = low,
        .positive_y = high,
        .negative_x_detail = shared::HeightTileSurfaceDetail::Coarse2,
        .positive_x_detail = shared::HeightTileSurfaceDetail::Coarse2,
    };
    auto const mesh = shared::HeightTileSurfaceMesher{}.build(tile, neighbors,
        shared::HeightTileSurfaceDetail::Coarse2);
    std::array<std::optional<shared::HeightTileSurfaceEdgeProfile>, 4> profiles;
    profiles[0] = shared::heightTileSurfaceEdgeProfile(mesh, shared::HeightTileSurfaceDirection::NegativeX);
    auto const aligned = shared::heightTileSurfaceFaceAreas(tile, neighbors, profiles);
    std::ranges::reverse(profiles[0]->top);
    auto const reversed = shared::heightTileSurfaceFaceAreas(tile, neighbors, profiles);
    EXPECT_EQ(aligned.maximum[0], 4.0);
    EXPECT_EQ(reversed.maximum[0], 28.0);
    EXPECT_EQ(profiles[0]->elevation,
        shared::heightTileSurfaceEdgeProfile(mesh, shared::HeightTileSurfaceDirection::NegativeX).elevation);
}

TEST(HeightTileSurfaceMesherTest, NonmonotoneCandidateAreasSelectTheCoarsestSafeDetail)
{
    static constexpr shared::HeightTileSurfaceProjection PROJECTION{ 90.0, 100U, 100U };
    static constexpr shared::HeightTileSurfaceBounds VIEWER{};
    static constexpr shared::HeightTileSurfaceBounds SURFACE{ .min_x_blocks = 400.0, .max_x_blocks = 400.0 };
    static constexpr shared::HeightTileSurfaceFaceAreas AREAS{ .maximum = { 2'048.0, 16.0, 8'192.0, 16'384.0 } };
    shared::HeightTileSurfaceLodPolicy const policy{ PROJECTION };
    EXPECT_EQ(policy.detailFor(VIEWER, SURFACE, AREAS), shared::HeightTileSurfaceDetail::Coarse4);
    EXPECT_EQ(policy.detailFor(VIEWER, SURFACE, AREAS, shared::HeightTileSurfaceDetail::Fine),
        shared::HeightTileSurfaceDetail::Coarse4);
    EXPECT_EQ(policy.detailFor(VIEWER, SURFACE, AREAS, shared::HeightTileSurfaceDetail::Coarse),
        shared::HeightTileSurfaceDetail::Coarse4);
}

TEST(HeightTileSurfaceMesherTest, CertifiedPublicCellPatchesProjectBelowTwoPixelsAtObliqueViews)
{
    static constexpr uint32_t SIDE = shared::HeightTile::SIDE_LENGTH;
    static constexpr std::array<shared::HeightTileSurfaceProjection, 2> PROJECTIONS{
        shared::HeightTileSurfaceProjection{ 70.0, 2'560U, 1'440U },
        shared::HeightTileSurfaceProjection{ 45.0, 3'840U, 2'160U },
    };
    static constexpr std::array<std::array<double, 3>, 3> EYES{
        std::array<double, 3>{ -40'000.0, 10'000.0, 10'000.0 },
        std::array<double, 3>{ 40'000.0, -10'000.0, 20'000.0 },
        std::array<double, 3>{ -20'000.0, -40'000.0, -10'000.0 },
    };
    shared::HeightTile tile;
    for (uint32_t y = 0U; y < SIDE; ++y) {
        for (uint32_t x = 0U; x < SIDE; ++x) {
            tile.heights[y * SIDE + x] = static_cast<uint16_t>(8U + ((x * 7U + y * 11U) % 5U));
        }
    }
    shared::HeightTileSurfaceNeighbors const neighbors{
        .negative_x = tile.heights, .positive_x = tile.heights,
        .negative_y = tile.heights, .positive_y = tile.heights,
    };
    auto const areas = shared::heightTileSurfaceFaceAreas(tile, neighbors);
    auto const bounds = shared::HeightTileSurfaceMesher::boundsForTile({}, tile, neighbors);
    for (auto const projection : PROJECTIONS) {
        shared::HeightTileSurfaceLodPolicy const policy{ projection };
        for (auto const eye : EYES) {
            shared::HeightTileSurfaceBounds const viewer{ eye[0], eye[0], eye[1], eye[1], eye[2], eye[2] };
            auto const detail = policy.detailFor(viewer, bounds, areas);
            ASSERT_NE(detail, shared::HeightTileSurfaceDetail::Fine);
            auto const found = std::ranges::find(shared::HeightTileSurfaceFaceAreas::DETAILS, detail);
            ASSERT_NE(found, shared::HeightTileSurfaceFaceAreas::DETAILS.end());
            uint32_t const level = static_cast<uint32_t>(found - shared::HeightTileSurfaceFaceAreas::DETAILS.begin());
            auto const mesh = shared::HeightTileSurfaceMesher{}.build(tile, neighbors, detail);
            for (auto const patch : cellPatches(mesh.quads, shared::HeightTileSurfaceFaceAreas::CELL_SIZES[level])) {
                EXPECT_LE(projectedPatchArea(patch, eye, projection), 2.0);
            }
        }
    }
}

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

TEST(HeightTileSurfaceMesherTest, CoarseDetailPreservesMacroReliefWhileReducingMicroGeometry)
{
    static constexpr std::array<uint16_t, 2U> MACRO_HEIGHTS{ 20U, 4U };
    static constexpr uint32_t COARSE_CELL_SIZE = 8U;
    shared::HeightTile tile{ .coordinate = { .x = 2, .y = 3 } };
    for (uint32_t y = 0U; y < shared::HeightTile::SIDE_LENGTH; ++y) {
        for (uint32_t x = 0U; x < shared::HeightTile::SIDE_LENGTH; ++x) {
            uint16_t const macro_height = MACRO_HEIGHTS[x / COARSE_CELL_SIZE];
            tile.heights[y * shared::HeightTile::SIDE_LENGTH + x] = macro_height + (x + y) % 3U;
        }
    }

    shared::HeightTileSurfaceMesher const mesher;
    shared::HeightTileSurfaceMesh const coarse = mesher.build(
        tile,
        {},
        shared::HeightTileSurfaceDetail::Coarse
    );
    shared::HeightTileSurfaceMesh const fine = mesher.build(tile);

    EXPECT_EQ(coarse.detail, shared::HeightTileSurfaceDetail::Coarse);
    std::array<std::optional<int32_t>, MACRO_HEIGHTS.size()> const macro_top_heights{
        topHeightAt(coarse, 35, 56),
        topHeightAt(coarse, 43, 56),
    };
    for (uint32_t band = 0U; band < MACRO_HEIGHTS.size(); ++band) {
        ASSERT_TRUE(macro_top_heights[band].has_value());
        EXPECT_GE(requiredValue(macro_top_heights[band]), MACRO_HEIGHTS[band]);
        EXPECT_LE(requiredValue(macro_top_heights[band]), MACRO_HEIGHTS[band] + 2U);
    }
    EXPECT_TRUE(hasQuad(
        coarse, 39, 48, 5, shared::HeightTileSurfaceDirection::PositiveX, 16U, 16U
    ));
    EXPECT_TRUE(hasQuad(
        coarse, 32, 48, 0, shared::HeightTileSurfaceDirection::NegativeX, 16U, 21U
    ));
    EXPECT_LT(coarse.quads.size(), fine.quads.size());
}

TEST(HeightTileSurfaceMesherTest, CoarseDetailBoundsGeometryForHighFrequencyTerrain)
{
    static constexpr uint16_t BASE_HEIGHT = 4U;
    static constexpr uint32_t HEIGHT_VARIATION = 8U;
    static constexpr uint32_t MAXIMUM_COARSE_QUAD_COUNT = 16U;
    shared::HeightTile tile{};
    for (uint32_t y = 0U; y < shared::HeightTile::SIDE_LENGTH; ++y) {
        for (uint32_t x = 0U; x < shared::HeightTile::SIDE_LENGTH; ++x) {
            tile.heights[y * shared::HeightTile::SIDE_LENGTH + x] =
                BASE_HEIGHT + (x + y) % HEIGHT_VARIATION;
        }
    }

    shared::HeightTileSurfaceMesh const coarse = shared::HeightTileSurfaceMesher{}.build(
        tile,
        {},
        shared::HeightTileSurfaceDetail::Coarse
    );

    EXPECT_LE(coarse.quads.size(), MAXIMUM_COARSE_QUAD_COUNT);
}

TEST(HeightTileSurfaceMesherTest, CoarseDetailDoesNotExpandAnIsolatedPeakAcrossItsCell)
{
    static constexpr uint16_t BASE_HEIGHT = 4U;
    static constexpr uint16_t ISOLATED_PEAK_HEIGHT = 28U;
    shared::HeightTile tile{};
    tile.heights.fill(BASE_HEIGHT);
    tile.heights[0U] = ISOLATED_PEAK_HEIGHT;

    shared::HeightTileSurfaceMesh const coarse = shared::HeightTileSurfaceMesher{}.build(
        tile,
        {},
        shared::HeightTileSurfaceDetail::Coarse
    );

    EXPECT_EQ(topHeightAt(coarse, 3, 3), BASE_HEIGHT);
}

TEST(HeightTileSurfaceMesherTest, DistantDetailRetainsBroadWaveSamplesAcrossTiles)
{
    static constexpr std::array<uint16_t, 8U> WAVE_TILE_HEIGHTS{
        4U, 8U, 12U, 16U, 12U, 8U, 4U, 8U,
    };
    static constexpr shared::HeightTileSurfaceDetail DISTANT_DETAIL =
        shared::HeightTileSurfaceDetail::Distant;
    static constexpr uint32_t MAXIMUM_DISTANT_QUAD_COUNT = 5U;
    for (uint32_t wave_index = 0U; wave_index < WAVE_TILE_HEIGHTS.size(); ++wave_index) {
        shared::HeightTile tile{
            .coordinate = { .x = static_cast<int32_t>(64U + wave_index), .y = 0 },
        };
        for (uint32_t y = 0U; y < shared::HeightTile::SIDE_LENGTH; ++y) {
            for (uint32_t x = 0U; x < shared::HeightTile::SIDE_LENGTH; ++x) {
                tile.heights[y * shared::HeightTile::SIDE_LENGTH + x] = static_cast<uint16_t>(
                    WAVE_TILE_HEIGHTS[wave_index] + (x + y) % 3U
                );
            }
        }

        shared::HeightTileSurfaceMesh const distant = shared::HeightTileSurfaceMesher{}.build(
            tile,
            {},
            DISTANT_DETAIL
        );

        EXPECT_EQ(distant.detail, DISTANT_DETAIL);
        EXPECT_EQ(
            topHeightAt(distant, tile.coordinate.x * 16 + 3, tile.coordinate.y * 16 + 5),
            WAVE_TILE_HEIGHTS[wave_index] + 1U
        );
        EXPECT_LE(distant.quads.size(), MAXIMUM_DISTANT_QUAD_COUNT);
    }
}

TEST(HeightTileSurfaceMesherTest, DetailPolicyStartsReductionAtTwoProjectedPixelsPerSourceFace)
{
    static constexpr double DEGREES_TO_RADIANS = 0.017'453'292'519'943'295'769;
    static constexpr double THRESHOLD_EPSILON = 1.0e-4;
    static constexpr shared::HeightTileSurfaceBounds VIEWER{
        .min_x_blocks = 0.0,
        .max_x_blocks = 0.0,
        .min_y_blocks = 0.0,
        .max_y_blocks = 0.0,
        .min_z_blocks = 0.0,
        .max_z_blocks = 0.0,
    };
    static constexpr std::array<shared::HeightTileSurfaceDetail, 4U> DETAIL_LEVELS{
        shared::HeightTileSurfaceDetail::Coarse2,
        shared::HeightTileSurfaceDetail::Coarse4,
        shared::HeightTileSurfaceDetail::Coarse,
        shared::HeightTileSurfaceDetail::Distant,
    };
    auto const makeProjection = [](double const horizontal_fov, uint32_t const width, uint32_t const height) {
        double const vertical_half_angle = std::atan(
            std::tan(horizontal_fov * DEGREES_TO_RADIANS / 2.0)
                * static_cast<double>(height) / static_cast<double>(width)
        );
        return shared::HeightTileSurfaceProjection{
            .vertical_fov_degrees = vertical_half_angle * 2.0 / DEGREES_TO_RADIANS,
            .viewport_width_pixels = width,
            .viewport_height_pixels = height,
        };
    };
    std::array<shared::HeightTileSurfaceProjection, 3U> const projections{
        makeProjection(80.0, 1'440U, 900U),
        makeProjection(90.0, 1'024U, 768U),
        makeProjection(80.0, 2'880U, 1'800U),
    };
    auto const thresholdDistance = [](shared::HeightTileSurfaceProjection const projection) {
        double const vertical_tangent = std::tan(
            projection.vertical_fov_degrees * DEGREES_TO_RADIANS / 2.0
        );
        double const horizontal_tangent = vertical_tangent
            * static_cast<double>(projection.viewport_width_pixels)
            / static_cast<double>(projection.viewport_height_pixels);
        double const focal_length_pixels = static_cast<double>(projection.viewport_height_pixels)
            / (2.0 * vertical_tangent);
        double const viewport_scale_squared = 1.0
            + horizontal_tangent * horizontal_tangent
            + vertical_tangent * vertical_tangent;
        return focal_length_pixels * std::pow(viewport_scale_squared, 0.75) / std::sqrt(2.0);
    };
    auto const projected_unit_face_area_bound = [](
        shared::HeightTileSurfaceProjection const projection,
        double const minimum_distance
    ) {
        double const vertical_tangent = std::tan(
            projection.vertical_fov_degrees * DEGREES_TO_RADIANS / 2.0
        );
        double const horizontal_tangent = vertical_tangent
            * static_cast<double>(projection.viewport_width_pixels)
            / static_cast<double>(projection.viewport_height_pixels);
        double const focal_length_pixels = static_cast<double>(projection.viewport_height_pixels)
            / (2.0 * vertical_tangent);
        double const viewport_scale_squared = 1.0
            + horizontal_tangent * horizontal_tangent
            + vertical_tangent * vertical_tangent;
        return focal_length_pixels * focal_length_pixels
            * std::pow(viewport_scale_squared, 1.5)
            / (minimum_distance * minimum_distance);
    };

    for (shared::HeightTileSurfaceProjection const projection : projections) {
        shared::HeightTileSurfaceLodPolicy const lod_policy{ projection };
        double const first_distance = thresholdDistance(projection);
        EXPECT_GT(first_distance, 0.0);
        for (uint32_t level = 0U; level < DETAIL_LEVELS.size(); ++level) {
            double const cell_size = static_cast<double>(2U << level);
            double const boundary = first_distance * cell_size;
            shared::HeightTileSurfaceBounds const at_boundary{
                .min_x_blocks = boundary,
                .max_x_blocks = boundary,
                .min_y_blocks = 0.0,
                .max_y_blocks = 0.0,
                .min_z_blocks = 0.0,
                .max_z_blocks = 0.0,
            };
            shared::HeightTileSurfaceBounds const just_nearer{
                .min_x_blocks = boundary - THRESHOLD_EPSILON,
                .max_x_blocks = boundary - THRESHOLD_EPSILON,
                .min_y_blocks = 0.0,
                .max_y_blocks = 0.0,
                .min_z_blocks = 0.0,
                .max_z_blocks = 0.0,
            };
            shared::HeightTileSurfaceDetail const preceding_detail = level == 0U
                ? shared::HeightTileSurfaceDetail::Fine
                : DETAIL_LEVELS[level - 1U];
            EXPECT_EQ(
                lod_policy.detailFor(VIEWER, just_nearer),
                preceding_detail
            );
            EXPECT_EQ(
                lod_policy.detailFor(VIEWER, at_boundary),
                DETAIL_LEVELS[level]
            );
            EXPECT_EQ(
                shared::HeightTileSurfaceMesher::detailFor(VIEWER, at_boundary, projection),
                lod_policy.detailFor(VIEWER, at_boundary)
            );
            double const represented_face_area_limit = 2.0 / (cell_size * cell_size);
            EXPECT_LE(projected_unit_face_area_bound(projection, boundary), represented_face_area_limit
                + THRESHOLD_EPSILON);
            EXPECT_GT(projected_unit_face_area_bound(projection, boundary - THRESHOLD_EPSILON),
                represented_face_area_limit - THRESHOLD_EPSILON);
        }
    }

    EXPECT_GT(thresholdDistance(projections[2]), thresholdDistance(projections[0]));
}

TEST(HeightTileSurfaceMesherTest, StatefulDetailSelectionUsesQualitySafeHysteresis)
{
    static constexpr double DEGREES_TO_RADIANS = 0.017'453'292'519'943'295'769;
    static constexpr double HYSTERESIS_BAND_PROBE = 1.05;
    static constexpr double COARSENING_PROBE = 1.20;
    static constexpr double THRESHOLD_EPSILON = 1.0e-4;
    static constexpr std::array<shared::HeightTileSurfaceProjection, 3U> PROJECTIONS{
        shared::HeightTileSurfaceProjection{
            .vertical_fov_degrees = 70.0,
            .viewport_width_pixels = 1'440U,
            .viewport_height_pixels = 900U,
        },
        shared::HeightTileSurfaceProjection{
            .vertical_fov_degrees = 70.0,
            .viewport_width_pixels = 2'880U,
            .viewport_height_pixels = 1'800U,
        },
        shared::HeightTileSurfaceProjection{
            .vertical_fov_degrees = 90.0,
            .viewport_width_pixels = 1'024U,
            .viewport_height_pixels = 768U,
        },
    };
    static constexpr std::array<double, 2U> HEIGHT_RANGES{ 0.0, 64.0 };
    auto const unitAreaCutoff = [](shared::HeightTileSurfaceProjection const projection) {
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
        return focal_length_pixels * std::pow(viewport_scale_squared, 0.75) / std::sqrt(2.0);
    };
    auto const cutoffDistance = [&unitAreaCutoff](
        shared::HeightTileSurfaceProjection const projection,
        double const height_range,
        double const cell_size
    ) {
        return unitAreaCutoff(projection) * std::sqrt(std::max(
            cell_size * cell_size,
            cell_size * height_range
        ));
    };

    for (shared::HeightTileSurfaceProjection const projection : PROJECTIONS) {
        shared::HeightTileSurfaceLodPolicy const policy{ projection };
        for (double const height_range : HEIGHT_RANGES) {
            double const first_cutoff = cutoffDistance(projection, height_range, 2.0);
            double const second_cutoff = cutoffDistance(projection, height_range, 4.0);
            shared::HeightTileSurfaceBounds const viewer{
                .min_x_blocks = 0.0,
                .max_x_blocks = 0.0,
                .min_y_blocks = 0.0,
                .max_y_blocks = 0.0,
                .min_z_blocks = height_range / 2.0,
                .max_z_blocks = height_range / 2.0,
            };
            auto const surfaceAt = [height_range](double const x) {
                return shared::HeightTileSurfaceBounds{
                    .min_x_blocks = x,
                    .max_x_blocks = x,
                    .min_y_blocks = 0.0,
                    .max_y_blocks = 0.0,
                    .min_z_blocks = 0.0,
                    .max_z_blocks = height_range,
                };
            };

            EXPECT_EQ(
                policy.detailFor(viewer, surfaceAt(first_cutoff * HYSTERESIS_BAND_PROBE),
                    shared::HeightTileSurfaceDetail::Fine),
                shared::HeightTileSurfaceDetail::Fine
            );
            EXPECT_EQ(
                policy.detailFor(viewer, surfaceAt(first_cutoff * HYSTERESIS_BAND_PROBE),
                    shared::HeightTileSurfaceDetail::Coarse2),
                shared::HeightTileSurfaceDetail::Coarse2
            );
            EXPECT_EQ(
                policy.detailFor(viewer, surfaceAt(first_cutoff - THRESHOLD_EPSILON),
                    shared::HeightTileSurfaceDetail::Coarse2),
                shared::HeightTileSurfaceDetail::Fine
            );
            EXPECT_EQ(
                policy.detailFor(viewer, surfaceAt(first_cutoff * COARSENING_PROBE),
                    shared::HeightTileSurfaceDetail::Fine),
                shared::HeightTileSurfaceDetail::Coarse2
            );
            EXPECT_EQ(
                policy.detailFor(viewer, surfaceAt(second_cutoff * HYSTERESIS_BAND_PROBE),
                    shared::HeightTileSurfaceDetail::Coarse2),
                shared::HeightTileSurfaceDetail::Coarse2
            );
            EXPECT_EQ(
                policy.detailFor(viewer, surfaceAt(second_cutoff * COARSENING_PROBE),
                    shared::HeightTileSurfaceDetail::Coarse2),
                shared::HeightTileSurfaceDetail::Coarse4
            );
        }
    }
}

TEST(HeightTileSurfaceMesherTest, DetailPolicyAccountsForTallCliffFaceArea)
{
    static constexpr shared::HeightTileSurfaceProjection PROJECTION{
        .vertical_fov_degrees = 90.0,
        .viewport_width_pixels = 100U,
        .viewport_height_pixels = 100U,
    };
    static constexpr shared::HeightTileSurfaceBounds VIEWER{
        .min_x_blocks = 0.0,
        .max_x_blocks = 0.0,
        .min_y_blocks = 0.0,
        .max_y_blocks = 0.0,
        .min_z_blocks = 0.0,
        .max_z_blocks = 0.0,
    };
    static constexpr shared::HeightTileSurfaceBounds FLAT_SURFACE{
        .min_x_blocks = 200.0,
        .max_x_blocks = 200.0,
        .min_y_blocks = 0.0,
        .max_y_blocks = 0.0,
        .min_z_blocks = 0.0,
        .max_z_blocks = 0.0,
    };
    static constexpr shared::HeightTileSurfaceBounds TALL_SURFACE{
        .min_x_blocks = 200.0,
        .max_x_blocks = 200.0,
        .min_y_blocks = 0.0,
        .max_y_blocks = 0.0,
        .min_z_blocks = 0.0,
        .max_z_blocks = 1'024.0,
    };
    static constexpr shared::HeightTileSurfaceBounds HIGH_VIEWER{
        .min_x_blocks = 0.0,
        .max_x_blocks = 0.0,
        .min_y_blocks = 0.0,
        .max_y_blocks = 0.0,
        .min_z_blocks = 1'000.0,
        .max_z_blocks = 1'000.0,
    };
    shared::HeightTileSurfaceLodPolicy const policy{ PROJECTION };

    EXPECT_EQ(
        policy.detailFor(VIEWER, FLAT_SURFACE),
        shared::HeightTileSurfaceDetail::Coarse2
    );
    EXPECT_EQ(
        policy.detailFor(VIEWER, TALL_SURFACE),
        shared::HeightTileSurfaceDetail::Fine
    );
    EXPECT_EQ(
        policy.detailFor(HIGH_VIEWER, FLAT_SURFACE),
        shared::HeightTileSurfaceDetail::Coarse
    );
    EXPECT_EQ(
        policy.detailFor(HIGH_VIEWER, TALL_SURFACE),
        shared::HeightTileSurfaceDetail::Fine
    );
}

TEST(HeightTileSurfaceMesherTest, DetailCutoversBoundProjectedTerrainFaceAreaForRelief)
{
    static constexpr double DEGREES_TO_RADIANS = 0.017'453'292'519'943'295'769;
    static constexpr double THRESHOLD_EPSILON = 1.0e-4;
    static constexpr std::array<shared::HeightTileSurfaceProjection, 3U> PROJECTIONS{
        shared::HeightTileSurfaceProjection{
            .vertical_fov_degrees = 80.0,
            .viewport_width_pixels = 1'440U,
            .viewport_height_pixels = 900U,
        },
        shared::HeightTileSurfaceProjection{
            .vertical_fov_degrees = 90.0,
            .viewport_width_pixels = 1'024U,
            .viewport_height_pixels = 768U,
        },
        shared::HeightTileSurfaceProjection{
            .vertical_fov_degrees = 80.0,
            .viewport_width_pixels = 2'880U,
            .viewport_height_pixels = 1'800U,
        },
    };
    static constexpr std::array<shared::HeightTileSurfaceDetail, 4U> DETAILS{
        shared::HeightTileSurfaceDetail::Coarse2,
        shared::HeightTileSurfaceDetail::Coarse4,
        shared::HeightTileSurfaceDetail::Coarse,
        shared::HeightTileSurfaceDetail::Distant,
    };
    static constexpr std::array<double, 4U> TERRAIN_HEIGHT_RANGES{ 0.0, 8.0, 64.0, 1'024.0 };
    auto const unit_face_area_bound = [](
        shared::HeightTileSurfaceProjection const projection,
        double const minimum_distance
    ) {
        double const vertical_tangent = std::tan(
            projection.vertical_fov_degrees * DEGREES_TO_RADIANS / 2.0
        );
        double const horizontal_tangent = vertical_tangent
            * static_cast<double>(projection.viewport_width_pixels)
            / static_cast<double>(projection.viewport_height_pixels);
        double const focal_length_pixels = static_cast<double>(projection.viewport_height_pixels)
            / (2.0 * vertical_tangent);
        double const viewport_scale_squared = 1.0
            + horizontal_tangent * horizontal_tangent
            + vertical_tangent * vertical_tangent;
        return focal_length_pixels * focal_length_pixels
            * std::pow(viewport_scale_squared, 1.5)
            / (minimum_distance * minimum_distance);
    };

    for (shared::HeightTileSurfaceProjection const projection : PROJECTIONS) {
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
        double const unit_face_cutoff = focal_length_pixels
            * std::pow(viewport_scale_squared, 0.75) / std::sqrt(2.0);
        shared::HeightTileSurfaceLodPolicy const policy{ projection };

        for (uint32_t level = 0U; level < DETAILS.size(); ++level) {
            double const current_cell_size = static_cast<double>(2U << level);
            for (double const terrain_height_range : TERRAIN_HEIGHT_RANGES) {
                double const terrain_face_area = std::max(
                    current_cell_size * current_cell_size,
                    current_cell_size * terrain_height_range
                );
                double const cutoff = unit_face_cutoff * std::sqrt(terrain_face_area);
                shared::HeightTileSurfaceBounds const viewer{
                    .min_x_blocks = 0.0,
                    .max_x_blocks = 0.0,
                    .min_y_blocks = 0.0,
                    .max_y_blocks = 0.0,
                    .min_z_blocks = terrain_height_range / 2.0,
                    .max_z_blocks = terrain_height_range / 2.0,
                };
                shared::HeightTileSurfaceBounds const at_cutoff{
                    .min_x_blocks = cutoff,
                    .max_x_blocks = cutoff,
                    .min_y_blocks = 0.0,
                    .max_y_blocks = 0.0,
                    .min_z_blocks = 0.0,
                    .max_z_blocks = terrain_height_range,
                };
                shared::HeightTileSurfaceBounds const immediately_nearer{
                    .min_x_blocks = cutoff - THRESHOLD_EPSILON,
                    .max_x_blocks = cutoff - THRESHOLD_EPSILON,
                    .min_y_blocks = 0.0,
                    .max_y_blocks = 0.0,
                    .min_z_blocks = 0.0,
                    .max_z_blocks = terrain_height_range,
                };

                shared::HeightTileSurfaceDetail const preceding_detail = level == 0U
                    ? shared::HeightTileSurfaceDetail::Fine
                    : DETAILS[level - 1U];
                EXPECT_EQ(
                    policy.detailFor(viewer, immediately_nearer),
                    preceding_detail
                );
                EXPECT_EQ(
                    policy.detailFor(viewer, at_cutoff),
                    DETAILS[level]
                );
                EXPECT_LE(
                    unit_face_area_bound(projection, cutoff) * terrain_face_area,
                    2.0 + THRESHOLD_EPSILON
                );
                EXPECT_GT(
                    unit_face_area_bound(projection, cutoff - THRESHOLD_EPSILON) * terrain_face_area,
                    2.0 - THRESHOLD_EPSILON
                );
            }
        }
    }
}

TEST(HeightTileSurfaceMesherTest, SurfaceBoundsIncludeTerrainNeighborHeights)
{
    static constexpr shared::HeightTileCoordinate CENTER{ .x = 0, .y = 0 };
    shared::HeightTile tile{
        .coordinate = { .x = 2, .y = 3 },
    };
    tile.heights.fill(4U);
    shared::HeightTile::Heights negative_x{};
    shared::HeightTile::Heights positive_x{};
    shared::HeightTile::Heights negative_y{};
    shared::HeightTile::Heights positive_y{};
    negative_x.fill(0U);
    positive_x.fill(12U);
    negative_y.fill(6U);
    positive_y.fill(8U);

    shared::HeightTileSurfaceBounds const bounds = shared::HeightTileSurfaceMesher::boundsForTile(
        CENTER,
        tile,
        {
            .negative_x = negative_x,
            .positive_x = positive_x,
            .negative_y = negative_y,
            .positive_y = positive_y,
        }
    );

    EXPECT_EQ(bounds.min_x_blocks, 32.0);
    EXPECT_EQ(bounds.max_x_blocks, 48.0);
    EXPECT_EQ(bounds.min_y_blocks, 48.0);
    EXPECT_EQ(bounds.max_y_blocks, 64.0);
    EXPECT_EQ(bounds.min_z_blocks, 0.0);
    EXPECT_EQ(bounds.max_z_blocks, 12.0);
}

TEST(HeightTileSurfaceMesherTest, SurfaceBoundsEncloseReducedNeighborInteriorAtTheSeam)
{
    static constexpr uint16_t HEIGHT = 1'024U;
    static constexpr uint32_t SIDE = shared::HeightTile::SIDE_LENGTH;
    static constexpr shared::HeightTileCoordinate CENTER{ .x = 0, .y = 0 };
    shared::HeightTile tile{ .coordinate = { .x = 200, .y = 0 } };
    tile.heights.fill(HEIGHT);
    shared::HeightTile::Heights neighbor{};
    for (uint32_t offset = 0U; offset < SIDE; ++offset) {
        neighbor[static_cast<shared::HeightTile::Heights::size_type>(offset) * SIDE] = HEIGHT;
    }
    shared::HeightTileSurfaceNeighbors const neighbors{
        .negative_x = tile.heights,
        .positive_x = neighbor,
        .negative_y = tile.heights,
        .positive_y = tile.heights,
        .positive_x_detail = shared::HeightTileSurfaceDetail::Coarse2,
    };

    shared::HeightTileSurfaceMesh const mesh = shared::HeightTileSurfaceMesher{}.build(
        tile, neighbors, shared::HeightTileSurfaceDetail::Coarse2
    );
    ASSERT_TRUE(hasQuad(
        mesh, 3'215, 0, HEIGHT / 2U, shared::HeightTileSurfaceDirection::PositiveX, SIDE, HEIGHT / 2U
    ));
    shared::HeightTileSurfaceBounds const bounds = shared::HeightTileSurfaceMesher::boundsForTile(
        CENTER, tile, neighbors
    );
    EXPECT_LE(bounds.min_z_blocks, HEIGHT / 2U);
    EXPECT_GE(bounds.max_z_blocks, HEIGHT);
    EXPECT_EQ(
        shared::HeightTileSurfaceMesher::detailFor(
            { .min_z_blocks = HEIGHT, .max_z_blocks = HEIGHT }, bounds
        ),
        shared::HeightTileSurfaceDetail::Fine
    );
}

TEST(HeightTileSurfaceMesherTest, InstalledEdgeElevationRangeIncludesTopAndWallProfiles)
{
    static constexpr uint16_t HEIGHT = 1'024U;
    static constexpr uint16_t WALL_BOTTOM = 512U;
    static constexpr uint16_t SIDE = shared::HeightTile::SIDE_LENGTH;
    shared::HeightTileSurfaceMesh const mesh{
        .quads = {
            { .z = HEIGHT - 1, .u_extent = SIDE, .v_extent = SIDE },
            {
                .x = SIDE - 1,
                .z = WALL_BOTTOM,
                .direction = shared::HeightTileSurfaceDirection::PositiveX,
                .u_extent = SIDE,
                .v_extent = HEIGHT - WALL_BOTTOM,
            },
        },
    };

    EXPECT_EQ(
        shared::heightTileSurfaceEdgeElevationRange(mesh, shared::HeightTileSurfaceDirection::PositiveX),
        (shared::HeightTileSurfaceElevationRange{ .minimum = WALL_BOTTOM, .maximum = HEIGHT })
    );
    EXPECT_EQ(
        shared::heightTileSurfaceEdgeElevationRange(mesh, shared::HeightTileSurfaceDirection::NegativeX),
        (shared::HeightTileSurfaceElevationRange{ .minimum = HEIGHT, .maximum = HEIGHT })
    );
    EXPECT_EQ(
        shared::heightTileSurfaceEdgeElevationRange({}, shared::HeightTileSurfaceDirection::NegativeX),
        shared::HeightTileSurfaceElevationRange{}
    );
}

TEST(HeightTileSurfaceMesherTest, SurfaceBoundsUseNearestWorldWrapAndAabbSeparation)
{
    static constexpr shared::HeightTileCoordinate CENTER{ .x = 0, .y = 0 };
    shared::HeightTileSurfaceBounds const wrapped = shared::HeightTileSurfaceMesher::boundsForTile(
        CENTER,
        { .x = 4'095, .y = 1 },
        12U,
        40U
    );
    EXPECT_EQ(wrapped.min_x_blocks, -16.0);
    EXPECT_EQ(wrapped.max_x_blocks, 0.0);
    EXPECT_EQ(wrapped.min_y_blocks, 16.0);
    EXPECT_EQ(wrapped.max_y_blocks, 32.0);
    EXPECT_EQ(wrapped.min_z_blocks, 12.0);
    EXPECT_EQ(wrapped.max_z_blocks, 40.0);

    static constexpr shared::HeightTileSurfaceProjection PROJECTION{
        .vertical_fov_degrees = 70.0,
        .viewport_width_pixels = 1'440U,
        .viewport_height_pixels = 900U,
    };
    double const vertical_tangent = std::tan(
        PROJECTION.vertical_fov_degrees * 0.017'453'292'519'943'295'769 / 2.0
    );
    double const horizontal_tangent = vertical_tangent
        * static_cast<double>(PROJECTION.viewport_width_pixels)
        / static_cast<double>(PROJECTION.viewport_height_pixels);
    double const viewport_scale_squared = 1.0
        + horizontal_tangent * horizontal_tangent
        + vertical_tangent * vertical_tangent;
    double const focal_length_pixels = static_cast<double>(PROJECTION.viewport_height_pixels)
        / (2.0 * vertical_tangent);
    double const boundary = 2.0 * (focal_length_pixels
        * std::pow(viewport_scale_squared, 0.75) / std::sqrt(2.0));
    shared::HeightTileSurfaceBounds const viewer{
        .min_x_blocks = 0.0,
        .max_x_blocks = 16.0,
        .min_y_blocks = 0.0,
        .max_y_blocks = 16.0,
        .min_z_blocks = 0.0,
        .max_z_blocks = 0.0,
    };
    shared::HeightTileSurfaceBounds const target{
        .min_x_blocks = 16.0 + boundary,
        .max_x_blocks = 32.0 + boundary,
        .min_y_blocks = 0.0,
        .max_y_blocks = 16.0,
        .min_z_blocks = 0.0,
        .max_z_blocks = 0.0,
    };
    EXPECT_EQ(
        shared::HeightTileSurfaceMesher::detailFor(viewer, target, PROJECTION),
        shared::HeightTileSurfaceDetail::Coarse2
    );
}

TEST(HeightTileSurfaceMesherTest, MixedDetailNeighborsPreserveTheSharedStepWall)
{
    static constexpr uint16_t BASE_HEIGHT = 15U;
    static constexpr uint16_t EDGE_HEIGHT = 16U;
    shared::HeightTile west{ .coordinate = { .x = 0, .y = 0 } };
    shared::HeightTile east{ .coordinate = { .x = 1, .y = 0 } };
    west.heights.fill(BASE_HEIGHT);
    east.heights.fill(BASE_HEIGHT);
    west.heights[15U] = EDGE_HEIGHT;
    east.heights[0U] = EDGE_HEIGHT;

    shared::HeightTileSurfaceMesh const west_mesh = shared::HeightTileSurfaceMesher{}.build(
        west,
        {
            .negative_x = {},
            .positive_x = east.heights,
            .negative_y = {},
            .positive_y = {},
            .positive_x_detail = shared::HeightTileSurfaceDetail::Fine,
        },
        shared::HeightTileSurfaceDetail::Coarse2
    );
    shared::HeightTileSurfaceMesh const east_mesh = shared::HeightTileSurfaceMesher{}.build(
        east,
        {
            .negative_x = west.heights,
            .positive_x = {},
            .negative_y = {},
            .positive_y = {},
            .negative_x_detail = shared::HeightTileSurfaceDetail::Coarse2,
        },
        shared::HeightTileSurfaceDetail::Fine
    );

    EXPECT_FALSE(hasQuad(
        west_mesh,
        16,
        0,
        BASE_HEIGHT,
        shared::HeightTileSurfaceDirection::PositiveX,
        1U,
        1U
    ));
    EXPECT_TRUE(hasQuad(
        east_mesh,
        16,
        0,
        BASE_HEIGHT,
        shared::HeightTileSurfaceDirection::NegativeX,
        1U,
        1U
    ));
}
