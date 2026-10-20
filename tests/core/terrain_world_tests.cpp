#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>

#include <generation/Generation.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace {

std::shared_ptr<generation::GlobalAtlas const> makeAtlas(
    int32_t const height,
    float const temperature = 12.0F,
    float const precipitation = 0.3F
)
{
    auto atlas = std::make_shared<generation::GlobalAtlas>();
    atlas->configuration.resolution = 256U;
    atlas->configuration.seed = 42U;
    atlas->sea_level = 0.0F;
    atlas->block_sea_level = 384U;
    atlas->max_height = 1'020U;
    atlas->vertical_scale = 1.0F;
    atlas->elevation.assign(256U * 256U, static_cast<float>(height - 384) + 0.5F);
    atlas->temperature.assign(256U * 256U, temperature);
    atlas->precipitation.assign(256U * 256U, precipitation);
    atlas->water_level.assign(256U * 256U, static_cast<float>(height - 384) + 0.5F);
    atlas->lake_mask.assign(256U * 256U, 0U);
    atlas->river_mask.assign(256U * 256U, 0U);
    atlas->ocean_mask.assign(256U * 256U, 0U);
    return atlas;
}

std::shared_ptr<shared::TerrainWorld const> makeWorld(
    int32_t const height,
    float const temperature = 12.0F,
    float const precipitation = 0.3F
)
{
    return std::make_shared<shared::TerrainWorld>(makeAtlas(height, temperature, precipitation));
}

shared::SurfaceTile sampleTile(shared::TerrainWorld const& world, int32_t const x = 0, int32_t const y = 0)
{
    auto const region = world.prepareRegion({ .origin_x = 0U, .origin_y = 0U, .extent = 64U });
    return world.sampleSurface(*region, { .x = x, .y = y, .z = 0 });
}

shared::Chunk expectMaterializedChunkMatchesReference(
    shared::TerrainWorld const& world,
    shared::SurfaceTile const& tile,
    shared::ChunkCoordinate const coordinate
)
{
    shared::Chunk::Blocks expected_blocks;
    for (uint8_t z = 0U; z < shared::Chunk::SIDE_LENGTH; ++z) {
        uint32_t const world_z = static_cast<uint32_t>(coordinate.z) * shared::Chunk::SIDE_LENGTH + z;
        for (uint8_t y = 0U; y < shared::Chunk::SIDE_LENGTH; ++y) {
            uint32_t const column_index = static_cast<uint32_t>(y) * shared::Chunk::SIDE_LENGTH;
            for (uint8_t x = 0U; x < shared::Chunk::SIDE_LENGTH; ++x) {
                uint32_t const block_index = static_cast<uint32_t>(z) * shared::Chunk::FACE_BLOCK_COUNT
                    + static_cast<uint32_t>(y) * shared::Chunk::SIDE_LENGTH
                    + x;
                expected_blocks[block_index] = world_z >= generation::WORLD_DEPTH_BLOCKS
                    ? shared::Block::Air
                    : tile.columns[column_index + x].materialAt(
                        static_cast<uint16_t>(world_z),
                        tile.block_sea_level
                    );
            }
        }
    }

    shared::Chunk const expected{ coordinate, expected_blocks };
    shared::Chunk const actual = world.materializeChunk(tile, coordinate);
    EXPECT_EQ(actual.contentIdentity(), expected.contentIdentity());
    for (uint8_t z = 0U; z < shared::Chunk::SIDE_LENGTH; ++z) {
        for (uint8_t y = 0U; y < shared::Chunk::SIDE_LENGTH; ++y) {
            for (uint8_t x = 0U; x < shared::Chunk::SIDE_LENGTH; ++x) {
                shared::BlockCoordinate const block_coordinate{ .x = x, .y = y, .z = z };
                EXPECT_EQ(actual.blockAt(block_coordinate), expected.blockAt(block_coordinate));
            }
        }
    }
    return actual;
}

template <typename TException, typename TOperation>
void expectTerrainError(TOperation&& operation, std::string_view const expected_message)
{
    try {
        std::forward<TOperation>(operation)();
        FAIL() << "expected terrain operation to fail";
    } catch (TException const& error) {
        EXPECT_EQ(std::string_view{ error.what() }, expected_message);
    } catch (...) {
        FAIL() << "terrain operation threw an unexpected exception type";
    }
}

} // namespace

TEST(TerrainWorldTest, MaterialIntervalsMatchInclusiveSeaAndBedrockRules)
{
    static constexpr uint16_t SEA_LEVEL = 384U;
    static constexpr std::array<int32_t, 5> HEIGHTS{
        SEA_LEVEL,
        SEA_LEVEL - 1,
        SEA_LEVEL - 3,
        SEA_LEVEL - 4,
        SEA_LEVEL - 5,
    };

    for (int32_t const height : HEIGHTS) {
        shared::TerrainColumn column;
        column.highest_solid_z = static_cast<uint16_t>(height);
        column.submerged = height < SEA_LEVEL;
        column.top_material = shared::Block::Grass;
        column.ground_material = shared::Block::Ice;
        if (column.submerged) {
            column.ground_first_z = SEA_LEVEL - 4U;
            column.ground_end_z = static_cast<uint16_t>(height + 1);
        } else {
            column.ground_first_z = static_cast<uint16_t>(height - 4);
            column.ground_end_z = static_cast<uint16_t>(height);
        }

        EXPECT_EQ(column.materialAt(0U, SEA_LEVEL), shared::Block::Bedrock);
        EXPECT_EQ(column.materialAt(static_cast<uint16_t>(height), SEA_LEVEL),
            height == SEA_LEVEL
                ? shared::Block::Grass
                : (height >= static_cast<int32_t>(SEA_LEVEL - 4U)
                    ? shared::Block::Ice
                    : shared::Block::Stone));
        EXPECT_EQ(column.materialAt(static_cast<uint16_t>(height + 1), SEA_LEVEL), shared::Block::Air);
    }

    shared::TerrainColumn const land{
        .highest_solid_z = SEA_LEVEL,
        .submerged = false,
        .top_material = shared::Block::Grass,
        .ground_material = shared::Block::Dirt,
        .ground_first_z = SEA_LEVEL - 4U,
        .ground_end_z = SEA_LEVEL,
    };
    EXPECT_EQ(land.materialAt(SEA_LEVEL, SEA_LEVEL), shared::Block::Grass);
    EXPECT_EQ(land.materialAt(SEA_LEVEL - 1U, SEA_LEVEL), shared::Block::Dirt);
    EXPECT_EQ(land.materialAt(SEA_LEVEL - 4U, SEA_LEVEL), shared::Block::Dirt);
    EXPECT_EQ(land.materialAt(SEA_LEVEL - 5U, SEA_LEVEL), shared::Block::Stone);

    shared::TerrainColumn const seabed{
        .highest_solid_z = SEA_LEVEL - 1U,
        .submerged = true,
        .top_material = shared::Block::Dirt,
        .ground_material = shared::Block::Ice,
        .ground_first_z = SEA_LEVEL - 4U,
        .ground_end_z = SEA_LEVEL,
    };
    EXPECT_EQ(seabed.materialAt(SEA_LEVEL, SEA_LEVEL), shared::Block::Air);
    EXPECT_EQ(seabed.materialAt(SEA_LEVEL - 1U, SEA_LEVEL), shared::Block::Ice);
    EXPECT_EQ(seabed.materialAt(SEA_LEVEL - 4U, SEA_LEVEL), shared::Block::Ice);
    EXPECT_EQ(seabed.materialAt(SEA_LEVEL - 5U, SEA_LEVEL), shared::Block::Stone);
}

TEST(TerrainWorldTest, SamplesClimateMaterialsAndMaterializesMatchingChunks)
{
    shared::TerrainWorld const warm_wet{ makeAtlas(384, 12.0F, 0.3F) };
    shared::SurfaceTile const warm_tile = sampleTile(warm_wet);
    ASSERT_EQ(warm_tile.columns.front().highest_solid_z, 384U);
    EXPECT_EQ(warm_tile.columns.front().top_material, shared::Block::Grass);
    EXPECT_EQ(warm_tile.columns.front().ground_material, shared::Block::Dirt);

    shared::Chunk const upper = warm_wet.materializeChunk(
        warm_tile,
        { .x = 0, .y = 0, .z = 24 }
    );
    EXPECT_EQ(upper.blockAt({ .x = 0, .y = 0, .z = 0 }), shared::Block::Grass);
    EXPECT_EQ(upper.blockAt({ .x = 0, .y = 0, .z = 1 }), shared::Block::Air);

    shared::Chunk const lower = warm_wet.materializeChunk(
        warm_tile,
        { .x = 0, .y = 0, .z = 23 }
    );
    EXPECT_EQ(lower.blockAt({ .x = 0, .y = 0, .z = 12 }), shared::Block::Dirt);
    EXPECT_EQ(lower.blockAt({ .x = 0, .y = 0, .z = 11 }), shared::Block::Stone);

    shared::TerrainWorld const cold_seabed{ makeAtlas(383, -5.0F, 0.3F) };
    shared::SurfaceTile const seabed_tile = sampleTile(cold_seabed);
    ASSERT_EQ(seabed_tile.columns.front().highest_solid_z, 383U);
    EXPECT_TRUE(seabed_tile.columns.front().submerged);
    EXPECT_EQ(seabed_tile.columns.front().top_material, shared::Block::Dirt);
    EXPECT_EQ(seabed_tile.columns.front().ground_material, shared::Block::Ice);

    shared::Chunk const seabed = cold_seabed.materializeChunk(
        seabed_tile,
        { .x = 0, .y = 0, .z = 23 }
    );
    EXPECT_EQ(seabed.blockAt({ .x = 0, .y = 0, .z = 15 }), shared::Block::Ice);
    EXPECT_EQ(seabed.blockAt({ .x = 0, .y = 0, .z = 12 }), shared::Block::Ice);

    shared::TerrainWorld const warm_dry{ makeAtlas(384, 12.0F, 0.1F) };
    shared::SurfaceTile const dry_tile = sampleTile(warm_dry);
    EXPECT_EQ(dry_tile.columns.front().top_material, shared::Block::Dirt);
    EXPECT_EQ(dry_tile.columns.front().ground_material, shared::Block::Dirt);

    auto lake_atlas = std::make_shared<generation::GlobalAtlas>(*makeAtlas(384));
    lake_atlas->lake_mask.front() = 1U;
    lake_atlas->water_level.front() = 10.75F;
    shared::TerrainWorld const inland_lake{ lake_atlas };
    shared::SurfaceTile const lake_tile = sampleTile(inland_lake);
    shared::TerrainColumn const lake_column = lake_tile.columns.front();
    EXPECT_TRUE(lake_column.water_present);
    EXPECT_EQ(lake_column.water_height_z, 394U);
    EXPECT_EQ(lake_column.top_material, shared::Block::Grass);
}

TEST(TerrainWorldTest, AppliesClimateMaterialThresholdsAtSourcePrecision)
{
    static constexpr float COLD_THRESHOLD_CELSIUS = -2.0F;
    static constexpr float PRECIPITATION_BELOW_THRESHOLD = 0.199'999'988'079'071'044'921'875F;
    static constexpr float PRECIPITATION_AT_FLOAT32_THRESHOLD = 0.20F;

    shared::TerrainWorld const threshold_cold{
        makeAtlas(384, COLD_THRESHOLD_CELSIUS, PRECIPITATION_AT_FLOAT32_THRESHOLD + 0.01F)
    };
    shared::TerrainWorld const just_warm{
        makeAtlas(384, COLD_THRESHOLD_CELSIUS + 0.001F, PRECIPITATION_AT_FLOAT32_THRESHOLD + 0.01F)
    };
    shared::TerrainWorld const threshold_dry{
        makeAtlas(384, COLD_THRESHOLD_CELSIUS + 0.001F, PRECIPITATION_BELOW_THRESHOLD)
    };
    shared::TerrainWorld const just_wet{
        makeAtlas(384, COLD_THRESHOLD_CELSIUS + 0.001F, PRECIPITATION_AT_FLOAT32_THRESHOLD)
    };

    EXPECT_EQ(sampleTile(threshold_cold).columns.front().top_material, shared::Block::Snow);
    EXPECT_EQ(sampleTile(threshold_cold).columns.front().ground_material, shared::Block::Ice);
    EXPECT_EQ(sampleTile(just_warm).columns.front().top_material, shared::Block::Grass);
    EXPECT_EQ(sampleTile(just_warm).columns.front().ground_material, shared::Block::Dirt);
    EXPECT_EQ(sampleTile(threshold_dry).columns.front().top_material, shared::Block::Dirt);
    EXPECT_EQ(sampleTile(just_wet).columns.front().top_material, shared::Block::Grass);
}

TEST(TerrainWorldTest, SamplesExactHMinusFiveThroughSeaLevelLayers)
{
    auto atlas = std::make_shared<generation::GlobalAtlas>(*makeAtlas(379));
    for (uint32_t y = 0U; y < atlas->configuration.resolution; ++y) {
        size_t const row = static_cast<size_t>(y) * atlas->configuration.resolution;
        atlas->elevation[row] = -4.5F;
        atlas->elevation[row + 1U] = 251.5F;
        atlas->water_level[row] = -4.5F;
        atlas->water_level[row + 1U] = 251.5F;
    }
    shared::TerrainWorld const world{ atlas };
    shared::SurfaceTile const tile = sampleTile(world);

    for (uint8_t x = 0U; x <= 5U; ++x) {
        shared::TerrainColumn const& column = tile.columns[x];
        uint16_t const expected_height = static_cast<uint16_t>(379U + x);
        ASSERT_EQ(column.highest_solid_z, expected_height);
        EXPECT_EQ(column.materialAt(expected_height, 384U),
            x == 5U ? shared::Block::Grass : (x == 0U ? shared::Block::Stone : shared::Block::Dirt));
        EXPECT_EQ(column.materialAt(380U, 384U),
            x == 0U ? shared::Block::Air : shared::Block::Dirt);
    }

    shared::Chunk const below_sea = world.materializeChunk(tile, { .x = 0, .y = 0, .z = 23 });
    EXPECT_EQ(below_sea.blockAt({ .x = 0, .y = 0, .z = 11 }), shared::Block::Stone);
    EXPECT_EQ(below_sea.blockAt({ .x = 1, .y = 0, .z = 12 }), shared::Block::Dirt);
    EXPECT_EQ(below_sea.blockAt({ .x = 4, .y = 0, .z = 15 }), shared::Block::Dirt);
    shared::Chunk const sea_level = world.materializeChunk(tile, { .x = 0, .y = 0, .z = 24 });
    EXPECT_EQ(sea_level.blockAt({ .x = 4, .y = 0, .z = 0 }), shared::Block::Air);
    EXPECT_EQ(sea_level.blockAt({ .x = 5, .y = 0, .z = 0 }), shared::Block::Grass);
}

TEST(TerrainWorldTest, UniformChunkMaterializationMatchesCanonicalBlocksAndHashes)
{
    static constexpr int32_t SURFACE_HEIGHT = 384;
    static constexpr uint16_t CHUNK_TOP_WORLD_Z = 367U;
    static constexpr uint16_t BELOW_CHUNK_TOP_WORLD_Z = 366U;
    static constexpr uint16_t SUBMERGED_GROUND_START_Z = 380U;
    static constexpr uint8_t BOUNDARY_COLUMN_X = 3U;
    static constexpr uint8_t LOWEST_BLOCK_Z = 0U;
    static constexpr uint8_t TOP_BLOCK_Z = 15U;

    std::shared_ptr<shared::TerrainWorld const> const world = makeWorld(SURFACE_HEIGHT);
    shared::SurfaceTile tile = sampleTile(*world);
    for (uint32_t index = 0U; index < shared::Chunk::FACE_BLOCK_COUNT; ++index) {
        shared::TerrainColumn& column = tile.columns[index];
        if (index % 3U == 1U) {
            column.precipitation = 0.1;
            column.top_material = shared::Block::Dirt;
        } else if (index % 3U == 2U) {
            column.temperature_celsius = -5.0;
            column.top_material = shared::Block::Snow;
            column.ground_material = shared::Block::Ice;
        }
    }

    shared::Chunk const uniform_stone = expectMaterializedChunkMatchesReference(
        *world,
        tile,
        { .x = 0, .y = 0, .z = 22 }
    );
    shared::Chunk const uniform_air = expectMaterializedChunkMatchesReference(
        *world,
        tile,
        { .x = 0, .y = 0, .z = 25 }
    );
    shared::Chunk const bedrock_boundary = expectMaterializedChunkMatchesReference(
        *world,
        tile,
        { .x = 0, .y = 0, .z = 0 }
    );
    shared::Chunk const ground_boundary = expectMaterializedChunkMatchesReference(
        *world,
        tile,
        { .x = 0, .y = 0, .z = 23 }
    );
    shared::Chunk const surface_boundary = expectMaterializedChunkMatchesReference(
        *world,
        tile,
        { .x = 0, .y = 0, .z = 24 }
    );

    shared::SurfaceTile at_chunk_top_tile = tile;
    shared::TerrainColumn& at_chunk_top_column = at_chunk_top_tile.columns[BOUNDARY_COLUMN_X];
    at_chunk_top_column.highest_solid_z = CHUNK_TOP_WORLD_Z;
    at_chunk_top_column.submerged = true;
    at_chunk_top_column.top_material = shared::Block::Dirt;
    at_chunk_top_column.ground_first_z = SUBMERGED_GROUND_START_Z;
    at_chunk_top_column.ground_end_z = SUBMERGED_GROUND_START_Z;
    shared::Chunk const at_chunk_top = expectMaterializedChunkMatchesReference(
        *world,
        at_chunk_top_tile,
        { .x = 0, .y = 0, .z = 22 }
    );

    shared::SurfaceTile below_chunk_top_tile = tile;
    shared::TerrainColumn& below_chunk_top_column = below_chunk_top_tile.columns[BOUNDARY_COLUMN_X];
    below_chunk_top_column.highest_solid_z = BELOW_CHUNK_TOP_WORLD_Z;
    below_chunk_top_column.submerged = true;
    below_chunk_top_column.top_material = shared::Block::Dirt;
    below_chunk_top_column.ground_first_z = SUBMERGED_GROUND_START_Z;
    below_chunk_top_column.ground_end_z = SUBMERGED_GROUND_START_Z;
    shared::Chunk const below_chunk_top = expectMaterializedChunkMatchesReference(
        *world,
        below_chunk_top_tile,
        { .x = 0, .y = 0, .z = 22 }
    );

    EXPECT_EQ(uniform_stone.blockAt({ .x = 0U, .y = 0U, .z = LOWEST_BLOCK_Z }), shared::Block::Stone);
    EXPECT_EQ(uniform_air.blockAt({ .x = 0U, .y = 0U, .z = LOWEST_BLOCK_Z }), shared::Block::Air);
    EXPECT_EQ(bedrock_boundary.blockAt({ .x = 0U, .y = 0U, .z = LOWEST_BLOCK_Z }), shared::Block::Bedrock);
    EXPECT_EQ(bedrock_boundary.blockAt({ .x = 0U, .y = 0U, .z = 1U }), shared::Block::Stone);
    EXPECT_EQ(ground_boundary.blockAt({ .x = 0U, .y = 0U, .z = 12U }), shared::Block::Dirt);
    EXPECT_EQ(ground_boundary.blockAt({ .x = 2U, .y = 0U, .z = 12U }), shared::Block::Ice);
    EXPECT_EQ(surface_boundary.blockAt({ .x = 0U, .y = 0U, .z = LOWEST_BLOCK_Z }), shared::Block::Grass);
    EXPECT_EQ(surface_boundary.blockAt({ .x = 1U, .y = 0U, .z = LOWEST_BLOCK_Z }), shared::Block::Dirt);
    EXPECT_EQ(surface_boundary.blockAt({ .x = 2U, .y = 0U, .z = LOWEST_BLOCK_Z }), shared::Block::Snow);
    EXPECT_EQ(surface_boundary.blockAt({ .x = 0U, .y = 0U, .z = TOP_BLOCK_Z }), shared::Block::Air);
    EXPECT_EQ(at_chunk_top.blockAt({ .x = BOUNDARY_COLUMN_X, .y = 0U, .z = TOP_BLOCK_Z }), shared::Block::Stone);
    EXPECT_EQ(below_chunk_top.blockAt({ .x = BOUNDARY_COLUMN_X, .y = 0U, .z = TOP_BLOCK_Z }), shared::Block::Air);
}

TEST(TerrainWorldTest, MaterializesFiveTypedLevelsAndPreservesSharedEdgesAndOrder)
{
    auto atlas = std::make_shared<generation::GlobalAtlas>(*makeAtlas(384));
    atlas->elevation[1U] = 0.75F;
    atlas->elevation[256U] = 0.25F;
    atlas->elevation[257U] = 0.625F;
    atlas->temperature[0U] = -5.0F;
    atlas->temperature[1U] = 5.0F;
    atlas->temperature[256U] = 18.0F;
    atlas->temperature[257U] = 0.0F;
    atlas->precipitation[0U] = 0.1F;
    atlas->precipitation[1U] = 0.4F;
    atlas->precipitation[256U] = 0.2F;
    atlas->precipitation[257U] = 0.8F;
    shared::TerrainWorld const world{ atlas };

    auto const left = world.prepareRegion({ .origin_x = 0U, .origin_y = 0U, .extent = 64U });
    auto const right = world.prepareRegion({ .origin_x = 64U, .origin_y = 0U, .extent = 64U });
    ASSERT_EQ(left->sampleCount(), 4'225U);
    ASSERT_NE(left->parent(), nullptr);
    ASSERT_EQ(left->parent()->key().extent, 256U);
    ASSERT_NE(left->parent()->parent(), nullptr);
    ASSERT_EQ(left->parent()->parent()->key().extent, 1'024U);
    ASSERT_EQ(left->parent()->parent()->parent()->key().extent, 4'096U);
    ASSERT_EQ(left->parent()->parent()->parent()->parent()->key().extent, 16'384U);

    shared::RegionalSample const left_edge = world.sampleRegion(*left, 64.0, 23.0);
    shared::RegionalSample const right_edge = world.sampleRegion(*right, 64.0, 23.0);
    EXPECT_EQ(left_edge.highest_solid_z, right_edge.highest_solid_z);
    EXPECT_EQ(left_edge.geological_elevation_km, right_edge.geological_elevation_km);
    EXPECT_EQ(left_edge.top_material, right_edge.top_material);

    shared::RegionalSample const periodic = world.sampleRegion(*left, 65'536.0, -65'536.0);
    shared::RegionalSample const origin = world.sampleRegion(*left, 0.0, 0.0);
    EXPECT_EQ(periodic.highest_solid_z, origin.highest_solid_z);
    EXPECT_EQ(periodic.geological_elevation_km, origin.geological_elevation_km);

    shared::TerrainWorld const reverse_order{ atlas };
    auto const right_first = reverse_order.prepareRegion({ .origin_x = 64U, .origin_y = 0U, .extent = 64U });
    auto const left_second = reverse_order.prepareRegion({ .origin_x = 0U, .origin_y = 0U, .extent = 64U });
    EXPECT_EQ(
        world.sampleRegion(*right_first, 64.0, 23.0).geological_elevation_km,
        world.sampleRegion(*right, 64.0, 23.0).geological_elevation_km
    );
    EXPECT_EQ(
        world.sampleRegion(*left_second, 23.0, 23.0).highest_solid_z,
        world.sampleRegion(*left, 23.0, 23.0).highest_solid_z
    );
}

TEST(TerrainWorldTest, RefinesOneRegionFromItsExactParent)
{
    shared::TerrainWorld const world{ makeAtlas(384) };
    std::shared_ptr<shared::RegionalArtifact const> const root = world.refineRegion(
        { .origin_x = 0U, .origin_y = 0U, .extent = 16'384U },
        nullptr
    );
    std::shared_ptr<shared::RegionalArtifact const> const parent = world.refineRegion(
        { .origin_x = 0U, .origin_y = 0U, .extent = 4'096U },
        root
    );

    ASSERT_EQ(root->parent(), nullptr);
    ASSERT_EQ(root->identity(), world.identity());
    ASSERT_EQ(parent->parent(), root);
    ASSERT_EQ(parent->identity(), world.identity());
    auto const cached = world.prepareRegion({ .origin_x = 0U, .origin_y = 0U, .extent = 4'096U });
    EXPECT_EQ(
        world.sampleRegion(*parent, 17.0, 23.0).geological_elevation_km,
        world.sampleRegion(*cached, 17.0, 23.0).geological_elevation_km
    );
}

TEST(TerrainWorldTest, RejectsRegionRefinementWithoutItsRequiredParent)
{
    shared::TerrainWorld const world{ makeAtlas(384) };
    expectTerrainError<std::invalid_argument>(
        [&world] {
            static_cast<void>(world.refineRegion(
                { .origin_x = 0U, .origin_y = 0U, .extent = 4'096U },
                nullptr
            ));
        },
        "terrain refinement requires its exact immediately coarser parent"
    );
}

TEST(TerrainWorldTest, RejectsRegionRefinementWithParentFromAnotherChild)
{
    shared::TerrainWorld const world{ makeAtlas(384) };
    auto const wrong_parent = world.refineRegion(
        { .origin_x = 16'384U, .origin_y = 0U, .extent = 16'384U },
        nullptr
    );
    expectTerrainError<std::invalid_argument>(
        [&world, &wrong_parent] {
            static_cast<void>(world.refineRegion(
                { .origin_x = 0U, .origin_y = 0U, .extent = 4'096U },
                wrong_parent
            ));
        },
        "terrain refinement requires its exact immediately coarser parent"
    );
}

TEST(TerrainWorldTest, RejectsRegionRefinementWithParentFromAnotherIdentity)
{
    auto const atlas = makeAtlas(384);
    shared::TerrainWorld const world{ atlas, 1U };
    shared::TerrainWorld const other_world{ atlas, 2U };
    std::shared_ptr<shared::RegionalArtifact const> const other_parent = other_world.refineRegion(
        { .origin_x = 0U, .origin_y = 0U, .extent = 16'384U },
        nullptr
    );

    expectTerrainError<std::invalid_argument>(
        [&world, &other_parent] {
            static_cast<void>(world.refineRegion(
                { .origin_x = 0U, .origin_y = 0U, .extent = 4'096U },
                other_parent
            ));
        },
        "terrain refinement requires its exact immediately coarser parent"
    );
}

TEST(TerrainWorldTest, RegionalSamplingAndMaterializationHonorCancellation)
{
    static constexpr std::string_view CANCELLATION_MESSAGE = "Terrain-v2 generation was cancelled";
    shared::TerrainWorld const world{ makeAtlas(384) };
    std::shared_ptr<shared::RegionalArtifact const> const leaf = world.prepareRegion(
        { .origin_x = 0U, .origin_y = 0U, .extent = 64U }
    );
    shared::SurfaceTile const tile = world.sampleSurface(*leaf, { .x = 0, .y = 0, .z = 0 });
    std::stop_source cancellation;
    cancellation.request_stop();
    std::stop_token const token = cancellation.get_token();

    expectTerrainError<generation::GenerationCancelled>(
        [&world, token] {
            static_cast<void>(world.refineRegion(
                { .origin_x = 0U, .origin_y = 0U, .extent = 16'384U },
                nullptr,
                token
            ));
        },
        CANCELLATION_MESSAGE
    );
    expectTerrainError<generation::GenerationCancelled>(
        [&world, token] {
            static_cast<void>(world.prepareRegion(
                { .origin_x = 0U, .origin_y = 0U, .extent = 64U },
                token
            ));
        },
        CANCELLATION_MESSAGE
    );
    expectTerrainError<generation::GenerationCancelled>(
        [&world, &leaf, token] {
            static_cast<void>(world.sampleRegion(*leaf, 0.0, 0.0, token));
        },
        CANCELLATION_MESSAGE
    );
    expectTerrainError<generation::GenerationCancelled>(
        [&world, &leaf, token] {
            static_cast<void>(world.sampleSurface(*leaf, { .x = 0, .y = 0, .z = 0 }, token));
        },
        CANCELLATION_MESSAGE
    );
    expectTerrainError<generation::GenerationCancelled>(
        [&world, token] {
            static_cast<void>(world.sampleSurface({ .x = 0, .y = 0, .z = 0 }, token));
        },
        CANCELLATION_MESSAGE
    );
    expectTerrainError<generation::GenerationCancelled>(
        [&world, &tile, token] {
            static_cast<void>(world.materializeChunk(tile, { .x = 0, .y = 0, .z = 0 }, token));
        },
        CANCELLATION_MESSAGE
    );
}

TEST(TerrainWorldTest, RejectsInvalidAtlasRegionAndMaterializationKeys)
{
    auto invalid_atlas = std::make_shared<generation::GlobalAtlas>();
    EXPECT_THROW(shared::TerrainWorld{ invalid_atlas }, std::invalid_argument);

    shared::TerrainWorld const world{ makeAtlas(384) };
    EXPECT_THROW(
        static_cast<void>(world.prepareRegion({ .origin_x = 1U, .origin_y = 0U, .extent = 64U })),
        std::invalid_argument
    );
    EXPECT_THROW(
        static_cast<void>(world.prepareRegion({ .origin_x = 0U, .origin_y = 0U, .extent = 32U })),
        std::invalid_argument
    );

    auto const region = world.prepareRegion({ .origin_x = 0U, .origin_y = 0U, .extent = 64U });
    shared::SurfaceTile const tile = world.sampleSurface(*region, { .x = 0, .y = 0, .z = 0 });
    EXPECT_NE(tile.columnAt({ .x = 0U, .y = 0U, .z = 0U }), nullptr);
    EXPECT_EQ(tile.columnAt({ .x = 0U, .y = 0U, .z = shared::Chunk::SIDE_LENGTH }), nullptr);
    EXPECT_THROW(
        static_cast<void>(world.materializeChunk(tile, { .x = 1, .y = 0, .z = 0 })),
        std::invalid_argument
    );
    EXPECT_THROW(
        static_cast<void>(world.materializeChunk(tile, { .x = 0, .y = 0, .z = 64 })),
        std::invalid_argument
    );
}

TEST(TerrainWorldTest, RejectsNoncanonicalIdentityAndVerticalBounds)
{
    auto wrong_sea = std::make_shared<generation::GlobalAtlas>(*makeAtlas(384));
    wrong_sea->block_sea_level += 1U;
    EXPECT_THROW(shared::TerrainWorld{ wrong_sea }, std::invalid_argument);

    auto wrong_maximum = std::make_shared<generation::GlobalAtlas>(*makeAtlas(384));
    wrong_maximum->max_height -= 1U;
    EXPECT_THROW(shared::TerrainWorld{ wrong_maximum }, std::invalid_argument);

    shared::TerrainWorld const world{ makeAtlas(384) };
    shared::SurfaceTile tile = sampleTile(world);
    ASSERT_TRUE(shared::isValidSurfaceTile(tile));

    tile.identity.seed += 1U;
    EXPECT_FALSE(shared::isValidSurfaceTile(tile));
    tile = sampleTile(world);
    tile.identity.configuration_identity = "terrain-v2;seed=42";
    EXPECT_FALSE(shared::isValidSurfaceTile(tile));
    tile = sampleTile(world);
    tile.block_sea_level += 1U;
    EXPECT_FALSE(shared::isValidSurfaceTile(tile));
    tile = sampleTile(world);
    tile.max_height -= 1U;
    EXPECT_FALSE(shared::isValidSurfaceTile(tile));
    tile = sampleTile(world);
    tile.columns.front().ground_end_z += 1U;
    EXPECT_FALSE(shared::isValidSurfaceTile(tile));
    tile = sampleTile(world);
    tile.columns.front().top_material = static_cast<shared::Block>(7U);
    EXPECT_FALSE(shared::isValidSurfaceTile(tile));
}

TEST(TerrainWorldTest, InjectedTerrainWorldSuppliesCanonicalCollisionChunks)
{
    auto const terrain_world = makeWorld(384, -5.0F, 0.3F);
    shared::SparseWorld world{ {}, terrain_world };

    EXPECT_EQ(world.blockAt({ .x = 0, .y = 0, .z = 384 }), shared::Block::Snow);
    EXPECT_EQ(world.blockAt({ .x = 0, .y = 0, .z = 380 }), shared::Block::Ice);
    EXPECT_EQ(world.blockAt({ .x = 0, .y = 0, .z = 379 }), shared::Block::Stone);
    EXPECT_EQ(world.residentChunkCount(), 2U);
}

TEST(TerrainWorldTest, FinalSurfaceAddsLocalDetailAfterTheFiveRegionalLevels)
{
    auto atlas = std::make_shared<generation::GlobalAtlas>(*makeAtlas(384));
    atlas->vertical_scale = 100.0;
    shared::TerrainWorld const world{ atlas };
    std::shared_ptr<shared::RegionalArtifact const> const leaf = world.prepareRegion(
        { .origin_x = 0U, .origin_y = 0U, .extent = shared::TERRAIN_REGION_EXTENTS.back() }
    );
    shared::SurfaceTile const tile = world.sampleSurface(
        *leaf,
        { .x = 0, .y = 0, .z = 0 }
    );

    shared::RegionalSample const root_sample = world.sampleRegion(*leaf, 0.0, 0.0);
    EXPECT_EQ(tile.columns.front().highest_solid_z, root_sample.highest_solid_z);
    EXPECT_EQ(tile.columns.front().geological_elevation_km, root_sample.geological_elevation_km);

    uint32_t changed_samples = 0U;
    uint32_t changed_index = 0U;
    for (uint32_t y = 0U; y < shared::Chunk::SIDE_LENGTH; ++y) {
        for (uint32_t x = 0U; x < shared::Chunk::SIDE_LENGTH; ++x) {
            uint32_t const index = y * shared::Chunk::SIDE_LENGTH + x;
            shared::RegionalSample const regional = world.sampleRegion(
                *leaf,
                static_cast<double>(x),
                static_cast<double>(y)
            );
            if (tile.columns[index].highest_solid_z != regional.highest_solid_z) {
                if (changed_samples == 0U) {
                    changed_index = index;
                }
                ++changed_samples;
            }
        }
    }
    std::shared_ptr<shared::RegionalArtifact const> const adjacent_leaf = world.prepareRegion(
        { .origin_x = 64U, .origin_y = 64U, .extent = shared::TERRAIN_REGION_EXTENTS.back() }
    );
    shared::SurfaceTile const adjacent_tile = world.sampleSurface(
        *adjacent_leaf,
        { .x = 4, .y = 4, .z = 0 }
    );
    shared::RegionalSample const shared_corner = world.sampleRegion(*adjacent_leaf, 64.0, 64.0);
    EXPECT_EQ(adjacent_tile.columns.front().highest_solid_z, shared_corner.highest_solid_z);
    EXPECT_EQ(adjacent_tile.columns.front().geological_elevation_km, shared_corner.geological_elevation_km);

    ASSERT_GT(changed_samples, 0U);
    shared::TerrainColumn const& changed_column = tile.columns[changed_index];
    shared::Chunk const chunk = world.materializeChunk(
        tile,
        { .x = 0, .y = 0, .z = changed_column.highest_solid_z / shared::Chunk::SIDE_LENGTH }
    );
    EXPECT_EQ(
        chunk.blockAt({
            .x = static_cast<uint8_t>(changed_index % shared::Chunk::SIDE_LENGTH),
            .y = static_cast<uint8_t>(changed_index / shared::Chunk::SIDE_LENGTH),
            .z = static_cast<uint8_t>(changed_column.highest_solid_z % shared::Chunk::SIDE_LENGTH),
        }),
        changed_column.top_material
    );
}

TEST(TerrainWorldTest, IdentityRequiresVersionedPortableShelfKernel)
{
    shared::TerrainIdentity const identity = makeWorld(384)->identity();
    EXPECT_TRUE(shared::isValidTerrainIdentity(identity));
    std::string const kernel = ";shelf-exp=exp64-to-f32-v1";
    ASSERT_TRUE(identity.configuration_identity.ends_with(kernel));

    auto missing_kernel = identity;
    missing_kernel.configuration_identity.resize(identity.configuration_identity.size() - kernel.size());
    EXPECT_FALSE(shared::isValidTerrainIdentity(missing_kernel));

    auto wrong_kernel = identity;
    wrong_kernel.configuration_identity.replace(
        identity.configuration_identity.size() - kernel.size(), kernel.size(), ";shelf-exp=host-f32-v1"
    );
    EXPECT_FALSE(shared::isValidTerrainIdentity(wrong_kernel));

    for (std::string_view const old_revision : { "model-revision=4", "model-revision=3" }) {
        auto invalid_identity = identity;
        size_t const revision = invalid_identity.configuration_identity.find("model-revision=5");
        ASSERT_NE(revision, std::string::npos);
        invalid_identity.configuration_identity.replace(revision, 16U, old_revision);
        EXPECT_FALSE(shared::isValidTerrainIdentity(invalid_identity));
    }
}
