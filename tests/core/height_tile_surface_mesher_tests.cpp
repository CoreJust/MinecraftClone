#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
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
        EXPECT_GE(*macro_top_heights[band], MACRO_HEIGHTS[band]);
        EXPECT_LE(*macro_top_heights[band], MACRO_HEIGHTS[band] + 2U);
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
            .positive_x = east.heights,
            .positive_x_detail = shared::HeightTileSurfaceDetail::Fine,
        },
        shared::HeightTileSurfaceDetail::Coarse2
    );
    shared::HeightTileSurfaceMesh const east_mesh = shared::HeightTileSurfaceMesher{}.build(
        east,
        {
            .negative_x = west.heights,
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
