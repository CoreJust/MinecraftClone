#include <client/Camera.hpp>
#include <client/ColumnMapPresentation.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>
#include <generation/Generation.hpp>

#include <glm/ext/vector_double3.hpp>
#include <gtest/gtest.h>

// The public dvec3 header includes GLM's inline vector implementation.
// IWYU pragma: no_include <glm/detail/type_vec3.inl>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using Palette = client::ColumnMapPalette;
using Rgba = client::ColumnMapRgba8;

[[nodiscard]] Palette testPalette()
{
    Palette palette{};
    palette.colors[static_cast<uint8_t>(shared::Block::Air)] = { 0U, 0U, 0U, 0U };
    palette.colors[static_cast<uint8_t>(shared::Block::Stone)] = { 12U, 34U, 56U, 255U };
    return palette;
}

void publishVerticalColumn(shared::SparseWorld& world, int32_t const solid_z)
{
    for (int32_t chunk_z = 0; chunk_z < 64; ++chunk_z) {
        shared::Chunk chunk{ { .x = 0, .y = 0, .z = chunk_z } };
        if (solid_z >= 0 && solid_z / shared::Chunk::SIDE_LENGTH == chunk_z) {
            static_cast<void>(chunk.setBlock({
                .x = 2U,
                .y = 3U,
                .z = static_cast<uint8_t>(solid_z % shared::Chunk::SIDE_LENGTH),
            }, shared::Block::Stone));
        }
        static_cast<void>(world.publishMaterializedChunk(chunk, 7U, 42U));
    }
}

[[nodiscard]] std::shared_ptr<generation::GlobalAtlas const> flatAtlas(int32_t const height)
{
    auto atlas = std::make_shared<generation::GlobalAtlas>();
    atlas->configuration.resolution = 256U;
    atlas->configuration.seed = 42U;
    atlas->sea_level = 0.0F;
    atlas->block_sea_level = 384U;
    atlas->max_height = generation::MAX_HEIGHT;
    atlas->vertical_scale = 1.0;
    uint32_t const sample_count = 256U * 256U;
    float const elevation = static_cast<float>(height)
        - static_cast<float>(atlas->block_sea_level) + 0.5F;
    atlas->elevation.assign(sample_count, elevation);
    atlas->temperature.assign(sample_count, 12.0F);
    atlas->precipitation.assign(sample_count, 0.3F);
    atlas->water_level.assign(sample_count, elevation);
    atlas->lake_mask.assign(sample_count, 0U);
    atlas->river_mask.assign(sample_count, 0U);
    atlas->ocean_mask.assign(sample_count, 0U);
    return atlas;
}

[[nodiscard]] std::unique_ptr<shared::SurfaceTile> sampleSurfaceOnHeap(
    shared::TerrainWorld const& terrain,
    shared::ChunkCoordinate const coordinate
)
{
    auto surface = std::make_unique<shared::SurfaceTile>();
    *surface = terrain.sampleSurface(coordinate);
    return surface;
}

TEST(ColumnMapPresentationTest, PixelUsesHighestMaterializedSolidAndCanonicalColor)
{
    static constexpr int32_t HIGHEST_SOLID_Z = 37;
    static constexpr client::WorldWindow WINDOW{
        .origin_x = 2,
        .origin_y = 3,
        .width = 1U,
        .height = 1U,
    };

    shared::SparseWorld world{
        shared::SparseWorldOptions{
            .seed = 42U,
            .max_resident_chunks = 80U,
            .revision = 7U,
        },
    };
    publishVerticalColumn(world, HIGHEST_SOLID_Z);
    client::ColumnMapPresentation presentation{ testPalette() };
    ASSERT_TRUE(presentation.setWindow(WINDOW));

    client::ColumnMapUpdate const update = presentation.update(world, 1U);

    EXPECT_EQ(update.scanned_columns, 1U);
    EXPECT_EQ(update.ready_columns, 1U);
    EXPECT_EQ(update.unknown_columns, 0U);
    ASSERT_EQ(presentation.planes().rgba.size(), 1U);
    EXPECT_EQ(presentation.planes().rgba[0], (Rgba{ 12U, 34U, 56U, 255U }));
    EXPECT_EQ(presentation.planes().material_ids[0], static_cast<uint8_t>(shared::Block::Stone));
    EXPECT_EQ(presentation.planes().coverage[0], client::ColumnCoverage::Ready);
    EXPECT_EQ(presentation.planes().highest_solid_z[0], HIGHEST_SOLID_Z);
}

TEST(ColumnMapPresentationTest, VerifiedSurfaceAndResidentTopChunkFinalizePixelWithoutUpperChunks)
{
    static constexpr int32_t HIGHEST_SOLID_Z = 900;
    static constexpr client::WorldWindow WINDOW{
        .origin_x = 0,
        .origin_y = 0,
        .width = 1U,
        .height = 1U,
    };

    shared::TerrainWorld const terrain{ flatAtlas(HIGHEST_SOLID_Z), 7U };
    auto const surface = sampleSurfaceOnHeap(terrain, { .x = 0, .y = 0, .z = 0 });
    uint16_t const highest_solid_z = surface->columns.front().highest_solid_z;
    auto const actual_top_chunk = std::make_unique<shared::Chunk>();
    *actual_top_chunk = terrain.materializeChunk(
        *surface,
        { .x = 0, .y = 0, .z = highest_solid_z / shared::Chunk::SIDE_LENGTH }
    );
    shared::SparseWorld world{
        shared::SparseWorldOptions{
            .seed = 42U,
            .max_resident_chunks = 8U,
            .revision = 7U,
        },
        shared::SparseWorldGenerationMode::MaterializedOnly,
    };
    ASSERT_TRUE(world.setTerrainIdentity(terrain.identity()));
    ASSERT_TRUE(world.publishSurfaceMetadata(*surface));
    ASSERT_TRUE(world.publishMaterializedChunk(*actual_top_chunk, 7U, 42U));
    EXPECT_EQ(
        world.queryBlock({ .x = 0, .y = 0, .z = shared::WorldExtent::DEPTH - 1U }).state,
        shared::GenerationState::Unknown
    );

    client::ColumnMapPresentation presentation{ testPalette() };
    ASSERT_TRUE(presentation.setWindow(WINDOW));
    client::ColumnMapUpdate const update = presentation.update(world, 1U);

    EXPECT_EQ(update.ready_columns, 1U);
    EXPECT_EQ(presentation.planes().coverage[0], client::ColumnCoverage::Ready);
    EXPECT_EQ(presentation.planes().highest_solid_z[0], highest_solid_z);
    EXPECT_EQ(presentation.planes().material_ids[0], static_cast<uint8_t>(shared::Block::Grass));

    ASSERT_TRUE(world.setBlock({ .x = 0, .y = 0, .z = highest_solid_z }, shared::Block::Stone));
    EXPECT_EQ(world.verifiedSurfaceColumn(0, 0), std::nullopt);
    static_cast<void>(presentation.update(world, 1U));
    EXPECT_EQ(presentation.planes().coverage[0], client::ColumnCoverage::Unknown);
}

TEST(ColumnMapPresentationTest, EvictingTheVerifiedHighestChunkReturnsPixelToUnknown)
{
    static constexpr int32_t HIGHEST_SOLID_Z = 900;
    static constexpr client::WorldWindow WINDOW{
        .origin_x = 0,
        .origin_y = 0,
        .width = 1U,
        .height = 1U,
    };

    shared::TerrainWorld const terrain{ flatAtlas(HIGHEST_SOLID_Z), 7U };
    shared::SurfaceTile const surface = terrain.sampleSurface({ .x = 0, .y = 0, .z = 0 });
    uint16_t const highest_solid_z = surface.columns.front().highest_solid_z;
    shared::ChunkCoordinate const highest_solid_chunk{
        .x = 0,
        .y = 0,
        .z = highest_solid_z / shared::Chunk::SIDE_LENGTH,
    };
    shared::SparseWorld world{
        shared::SparseWorldOptions{
            .seed = 42U,
            .max_resident_chunks = 1U,
            .revision = 7U,
        },
        shared::SparseWorldGenerationMode::MaterializedOnly,
    };
    ASSERT_TRUE(world.setTerrainIdentity(terrain.identity()));
    ASSERT_TRUE(world.publishSurfaceMetadata(surface));
    ASSERT_TRUE(world.publishMaterializedChunk(
        terrain.materializeChunk(surface, highest_solid_chunk),
        7U,
        42U
    ));

    client::ColumnMapPresentation presentation{ testPalette() };
    ASSERT_TRUE(presentation.setWindow(WINDOW));
    static_cast<void>(presentation.update(world, 1U));
    ASSERT_EQ(presentation.planes().coverage[0], client::ColumnCoverage::Ready);

    ASSERT_TRUE(world.evict(highest_solid_chunk));
    static_cast<void>(presentation.update(world, 1U));

    EXPECT_EQ(presentation.planes().coverage[0], client::ColumnCoverage::Unknown);
    EXPECT_EQ(presentation.planes().highest_solid_z[0], -1);
    EXPECT_EQ(presentation.planes().material_ids[0], static_cast<uint8_t>(shared::Block::Air));
    EXPECT_EQ(presentation.planes().rgba[0], (Rgba{ 0U, 0U, 0U, 0U }));
}

TEST(ColumnMapPresentationTest, UnknownUpperChunksCannotBeReplacedByALowerLoadedBlock)
{
    static constexpr client::WorldWindow WINDOW{
        .origin_x = 2,
        .origin_y = 3,
        .width = 1U,
        .height = 1U,
    };

    shared::SparseWorld world{
        shared::SparseWorldOptions{
            .seed = 42U,
            .max_resident_chunks = 8U,
            .revision = 7U,
        },
    };
    shared::Chunk lower_chunk{ { .x = 0, .y = 0, .z = 0 } };
    ASSERT_TRUE(lower_chunk.setBlock({ .x = 2U, .y = 3U, .z = 5U }, shared::Block::Stone));
    ASSERT_TRUE(world.publishMaterializedChunk(lower_chunk, 7U, 42U));

    client::ColumnMapPresentation presentation{ testPalette() };
    ASSERT_TRUE(presentation.setWindow(WINDOW));
    client::ColumnMapUpdate const update = presentation.update(world, 1U);

    EXPECT_EQ(update.ready_columns, 0U);
    EXPECT_EQ(update.unknown_columns, 1U);
    EXPECT_EQ(presentation.planes().coverage[0], client::ColumnCoverage::Unknown);
    EXPECT_EQ(presentation.planes().highest_solid_z[0], -1);
    EXPECT_EQ(presentation.planes().material_ids[0], static_cast<uint8_t>(shared::Block::Air));
    EXPECT_EQ(presentation.planes().rgba[0], (Rgba{ 0U, 0U, 0U, 0U }));
}

TEST(ColumnMapPresentationTest, HeightTilePreviewDoesNotCountAsAuthoritativeColumnCoverage)
{
    static constexpr client::WorldWindow WINDOW{
        .origin_x = 2,
        .origin_y = 3,
        .width = 1U,
        .height = 1U,
    };

    shared::SparseWorld world{
        shared::SparseWorldOptions{
            .seed = 42U,
            .max_resident_chunks = 8U,
            .revision = 7U,
        },
    };
    shared::HeightTile preview{};
    preview.coordinate = { .x = 0, .y = 0 };
    preview.heights.fill(37U);
    ASSERT_TRUE(world.publishPreview({ .x = 0, .y = 0, .z = 0 }, preview, 7U, 42U));

    client::ColumnMapPresentation presentation{ testPalette() };
    ASSERT_TRUE(presentation.setWindow(WINDOW));
    client::ColumnMapUpdate const update = presentation.update(world, 1U);

    EXPECT_EQ(update.ready_columns, 0U);
    EXPECT_EQ(update.unknown_columns, 1U);
    EXPECT_EQ(presentation.planes().coverage[0], client::ColumnCoverage::Unknown);
    EXPECT_EQ(presentation.planes().rgba[0], (Rgba{ 0U, 0U, 0U, 0U }));
}

TEST(ColumnMapPresentationTest, UpdateBudgetIsBoundedAndInvalidWindowLeavesTheCurrentTileIntact)
{
    static constexpr client::WorldWindow WINDOW{
        .origin_x = -1,
        .origin_y = 65'535,
        .width = 2U,
        .height = 2U,
    };
    static constexpr client::WorldWindow INVALID_WINDOW{
        .origin_x = 0,
        .origin_y = 0,
        .width = 257U,
        .height = 1U,
    };

    shared::SparseWorld world{
        shared::SparseWorldOptions{
            .seed = 42U,
            .max_resident_chunks = 8U,
            .revision = 7U,
        },
    };
    client::ColumnMapPresentation presentation{ testPalette() };
    ASSERT_TRUE(presentation.setWindow(WINDOW));

    client::ColumnMapUpdate const first = presentation.update(world, 2U);
    EXPECT_EQ(first.scanned_columns, 2U);
    EXPECT_EQ(first.unknown_columns, 2U);
    EXPECT_FALSE(presentation.setWindow(INVALID_WINDOW));
    EXPECT_EQ(presentation.window(), WINDOW);
    EXPECT_EQ(presentation.planes().coverage.size(), 4U);
    EXPECT_EQ(presentation.nextColumnIndex(), 2U);

    client::ColumnMapUpdate const second = presentation.update(world, 1U);
    EXPECT_EQ(second.scanned_columns, 1U);
    EXPECT_EQ(presentation.nextColumnIndex(), 3U);
}

TEST(ColumnMapPresentationTest, LargeTilesStillUseTheFixedPerCallColumnBudget)
{
    static constexpr client::WorldWindow WINDOW{
        .origin_x = 0,
        .origin_y = 0,
        .width = client::ColumnMapPresentation::MAX_TILE_SIDE,
        .height = client::ColumnMapPresentation::MAX_TILE_SIDE,
    };
    static constexpr uint32_t REQUESTED_COLUMNS = 65'536U;

    shared::SparseWorld world{
        shared::SparseWorldOptions{
            .seed = 42U,
            .max_resident_chunks = 8U,
            .revision = 7U,
        },
    };
    client::ColumnMapPresentation presentation{ testPalette() };
    ASSERT_TRUE(presentation.setWindow(WINDOW));

    client::ColumnMapUpdate const update = presentation.update(world, REQUESTED_COLUMNS);

    EXPECT_EQ(update.scanned_columns, client::ColumnMapPresentation::MAX_COLUMNS_PER_UPDATE);
    EXPECT_EQ(update.unknown_columns, client::ColumnMapPresentation::MAX_COLUMNS_PER_UPDATE);
    EXPECT_EQ(presentation.nextColumnIndex(), client::ColumnMapPresentation::MAX_COLUMNS_PER_UPDATE);
}

TEST(ColumnMapPresentationTest, WorldAlignedColumnPixelDoesNotChangeWithCameraOrientation)
{
    static constexpr int32_t HIGHEST_SOLID_Z = 37;
    static constexpr client::WorldWindow WINDOW{
        .origin_x = 2,
        .origin_y = 3,
        .width = 1U,
        .height = 1U,
    };
    static constexpr std::array<uint8_t, 4> EXPECTED_RGBA{ 12U, 34U, 56U, 255U };

    shared::SparseWorld world{
        shared::SparseWorldOptions{
            .seed = 42U,
            .max_resident_chunks = 80U,
            .revision = 7U,
        },
    };
    publishVerticalColumn(world, HIGHEST_SOLID_Z);
    client::ColumnMapPresentation presentation{ testPalette() };
    ASSERT_TRUE(presentation.setWindow(WINDOW));

    client::Camera const first_camera{ client::CameraPose{
        .angles = { .yaw_degrees = 0.0, .pitch_degrees = 0.0 },
    } };
    EXPECT_EQ(first_camera.forward(), (glm::dvec3{ 0.0, 1.0, 0.0 }));
    static_cast<void>(presentation.update(world, 1U));
    std::vector<Rgba> const first_pixels = presentation.planes().rgba;

    client::Camera const second_camera{ client::CameraPose{
        .angles = { .yaw_degrees = 90.0, .pitch_degrees = 25.0 },
    } };
    EXPECT_NE(first_camera.forward(), second_camera.forward());
    static_cast<void>(presentation.update(world, 1U));

    EXPECT_EQ(first_pixels[0], EXPECTED_RGBA);
    EXPECT_EQ(presentation.planes().rgba[0], first_pixels[0]);
}

} // namespace
