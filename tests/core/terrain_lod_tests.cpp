#include <shared/world/Chunk.hpp>
#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <generation/Generation.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace {

struct LodWorldParameters final {
    bool ocean = false;
    float elevation = 1.5F;
    float temperature = 12.0F;
    double scale = 10.0;
};

std::shared_ptr<shared::TerrainWorld const> makeLodWorld(
    uint32_t const resolution = 256U,
    LodWorldParameters const parameters = {}
)
{
    auto atlas = std::make_shared<generation::GlobalAtlas>();
    atlas->configuration.resolution = resolution;
    atlas->vertical_scale = parameters.scale;
    std::vector<float>::size_type const count =
        static_cast<std::vector<float>::size_type>(resolution) * resolution;
    atlas->elevation.assign(count, parameters.ocean ? -parameters.elevation : parameters.elevation);
    atlas->temperature.assign(count, parameters.temperature);
    atlas->precipitation.assign(count, 0.3F);
    atlas->water_level.assign(count, 0.0F);
    atlas->ocean_mask.assign(count, parameters.ocean ? 1U : 0U);
    atlas->lake_mask.assign(count, 0U);
    atlas->river_mask.assign(count, 0U);
    return std::make_shared<shared::TerrainWorld const>(atlas);
}

} // namespace

TEST(TerrainLodTest, RegistersResolutionDependentSpacingAndSeparateVisualIdentity)
{
    for (uint32_t const resolution : {256U, 512U}) {
        auto const world = makeLodWorld(resolution);
        EXPECT_EQ(shared::terrainLodSpacing(resolution, shared::TerrainLodLevel::L0), 65'536U / resolution);
        EXPECT_EQ(shared::terrainLodSpacing(resolution, shared::TerrainLodLevel::L1), 16'384U / resolution);
        EXPECT_EQ(shared::terrainLodSpacing(resolution, shared::TerrainLodLevel::L2), 4'096U / resolution);
        EXPECT_EQ(shared::terrainLodSpacing(resolution, shared::TerrainLodLevel::L3), 1U);
        auto const atlas = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
        EXPECT_EQ(atlas->terrain_identity, world->identity());
        EXPECT_EQ(atlas->model_identity, "lod-canonical-r6-height-v2");
        EXPECT_EQ(atlas->side, resolution + 1U);
        EXPECT_EQ(atlas->nodes.front(), atlas->nodes.back());
        for (uint32_t row = 0U; row < atlas->side; ++row) {
            size_t const row_offset = static_cast<size_t>(row) * atlas->side;
            EXPECT_EQ(atlas->nodes[row_offset], atlas->nodes[row_offset + resolution]);
            EXPECT_EQ(atlas->nodes[row], atlas->nodes[resolution * atlas->side + row]);
        }
    }
}

TEST(TerrainLodTest, AdjacentWindowsShareEdgesIndependentOfConstructionOrder)
{
    auto const world = makeLodWorld();
    auto const right = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L2, 256U, 0U, 256U);
    auto const left = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L2, 0U, 0U, 256U);
    auto const again = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L2, 256U, 0U, 256U);
    EXPECT_EQ(right->nodes, again->nodes);
    for (uint32_t row = 0U; row < left->side; ++row) {
            size_t const left_row_offset = static_cast<size_t>(row) * left->side;
            size_t const right_row_offset = static_cast<size_t>(row) * right->side;
            EXPECT_EQ(left->nodes[left_row_offset + left->side - 1U], right->nodes[right_row_offset]);
    }
    auto const seam = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L2, 65'280U, 0U, 256U);
    for (uint32_t row = 0U; row < left->side; ++row) {
        size_t const seam_row_offset = static_cast<size_t>(row) * seam->side;
        size_t const left_row_offset = static_cast<size_t>(row) * left->side;
        EXPECT_EQ(seam->nodes[seam_row_offset + seam->side - 1U], left->nodes[left_row_offset]);
    }
}

TEST(TerrainLodTest, RefinementPreservesParentAnchorsAndExposesMorphHeight)
{
    for (uint32_t const resolution : {256U, 512U}) {
        auto const world = makeLodWorld(resolution);
        auto const l0 = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
        auto const l1 = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L1, 32'512U, 32'512U, 512U);
        auto const l2 = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L2, 32'512U, 32'512U, 512U);
        for (auto const& node : l1->nodes) {
            EXPECT_DOUBLE_EQ(node.sample.parent_height, shared::sampleTerrainLod(*l0, node.key.x, node.key.y).height);
            if (node.key.x % l0->spacing == 0U && node.key.y % l0->spacing == 0U) {
                EXPECT_DOUBLE_EQ(node.sample.height, node.sample.parent_height);
            }
        }
        for (auto const& node : l2->nodes) {
            EXPECT_DOUBLE_EQ(node.sample.parent_height, shared::sampleTerrainLod(*l1, node.key.x, node.key.y).height);
            if (node.key.x % l1->spacing == 0U && node.key.y % l1->spacing == 0U) {
                EXPECT_DOUBLE_EQ(node.sample.height, node.sample.parent_height);
            }
        }
    }
}

TEST(TerrainLodTest, RejectsUnboundedOrUnalignedWindowsAndHonorsCancellation)
{
    auto const world = makeLodWorld();
    EXPECT_THROW(static_cast<void>(shared::buildTerrainLod(*world, shared::TerrainLodLevel::L2)),
        std::invalid_argument);
    EXPECT_THROW(static_cast<void>(shared::buildTerrainLod(*world, shared::TerrainLodLevel::L1, 1U, 0U, 256U)),
        std::invalid_argument);
    EXPECT_THROW(static_cast<void>(shared::terrainLodSpacing(128U, shared::TerrainLodLevel::L0)),
        std::invalid_argument);
    EXPECT_THROW(static_cast<void>(shared::terrainLodSpacing(256U, static_cast<shared::TerrainLodLevel>(4U))),
        std::invalid_argument);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW(static_cast<void>(shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0,
        0U, 0U, 65'536U, stop.get_token())),
        generation::GenerationCancelled);
    auto const tile = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L2, 0U, 0U, 256U);
    EXPECT_THROW(static_cast<void>(shared::sampleTerrainLod(*tile, 300.0, 0.0)), std::invalid_argument);
}

TEST(TerrainLodTest, FinestLevelUsesActualCanonicalChunkColumns)
{
    auto const world = makeLodWorld();
    auto const artifact = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L3, 32'768U, 32'768U, 1U);
    auto const surface = world->sampleSurface({.x = 2'048, .y = 2'048, .z = 0});
    EXPECT_DOUBLE_EQ(artifact->nodes.front().sample.height, surface.columns.front().highest_solid_z);
    EXPECT_EQ(artifact->nodes.front().sample.top_material, surface.columns.front().materialAt(
        surface.columns.front().highest_solid_z, generation::BLOCK_SEA_LEVEL));
    EXPECT_EQ(artifact->nodes.front().sample.water_present, surface.columns.front().water_present);
}

TEST(TerrainLodTest, KeepsOceanClassificationAndWaterLevelAcrossRefinement)
{
    auto const world = makeLodWorld(256U, { .ocean = true });
    for (auto const level : {shared::TerrainLodLevel::L1, shared::TerrainLodLevel::L2}) {
        auto const artifact = shared::buildTerrainLod(*world, level, 32'640U, 32'640U, 256U);
        for (auto const& node : artifact->nodes) {
            EXPECT_TRUE(node.sample.water_present);
            EXPECT_EQ(node.sample.water_height, generation::BLOCK_SEA_LEVEL);
            EXPECT_LT(node.sample.height, generation::BLOCK_SEA_LEVEL);
            EXPECT_EQ(node.sample.top_material, shared::Block::Stone);
        }
    }
}

TEST(TerrainLodTest, SurfaceMaterialsMatchCanonicalLayersAtOwnHeightAndClimateBoundary)
{
    struct Case final {
        uint16_t height;
        float temperature;
        shared::Block expected;
    };
    for (Case const test_case : {
        Case{379U, 12.0F, shared::Block::Stone},
        Case{380U, 12.0F, shared::Block::Dirt},
        Case{383U, 12.0F, shared::Block::Dirt},
        Case{379U, -2.0F, shared::Block::Stone},
        Case{380U, -2.0F, shared::Block::Ice},
        Case{384U, -2.0F, shared::Block::Snow},
        Case{384U, -1.999F, shared::Block::Grass},
    }) {
        bool const submerged = test_case.height < generation::BLOCK_SEA_LEVEL;
        float const elevation = static_cast<float>(test_case.height) - generation::BLOCK_SEA_LEVEL + 0.5F;
        auto const world = makeLodWorld(256U, {
            .ocean = submerged,
            .elevation = submerged ? -elevation : elevation,
            .temperature = test_case.temperature,
            .scale = 1.0,
        });
        auto const surface = world->sampleSurface({.x = 2'048, .y = 2'048, .z = 0});
        auto const& canonical = surface.columns.front();
        ASSERT_EQ(canonical.highest_solid_z, test_case.height);
        auto const canonical_material = canonical.materialAt(canonical.highest_solid_z, generation::BLOCK_SEA_LEVEL);
        ASSERT_EQ(canonical_material, test_case.expected);
        for (auto const level : {shared::TerrainLodLevel::L0, shared::TerrainLodLevel::L1,
            shared::TerrainLodLevel::L2, shared::TerrainLodLevel::L3}) {
            auto const artifact = level == shared::TerrainLodLevel::L0 ? shared::buildTerrainLod(*world, level)
                : shared::buildTerrainLod(
                    *world, level, 32'768U, 32'768U, level == shared::TerrainLodLevel::L1 ? 64U : 16U
                );
            auto const sample = shared::sampleTerrainLod(*artifact, 32'768.0, 32'768.0);
            EXPECT_DOUBLE_EQ(sample.height, test_case.height);
            EXPECT_EQ(sample.top_material, canonical_material);
            EXPECT_EQ(sample.water_present, canonical.water_present);
            EXPECT_EQ(sample.water_height, canonical.water_height_z);
        }
    }
}

TEST(TerrainLodTest, InterpolatedClimateAndMaterialMatchCanonicalBoundaryAtEveryCoarseLevel)
{
    for (uint32_t const resolution : {256U, 512U}) {
        auto atlas = std::make_shared<generation::GlobalAtlas>(*makeLodWorld(resolution)->atlas());
        for (uint32_t y = 0U; y < resolution; ++y) {
            for (uint32_t x = 0U; x < resolution; ++x) {
                float const fx = static_cast<float>(x % 2U);
                float const fy = static_cast<float>(y % 2U);
                atlas->temperature[y * resolution + x] = -4.0F + 6.0F * fx + 4.0F * fy - 7.0F * fx * fy;
                atlas->precipitation[y * resolution + x] = 0.1F + 0.2F * fx + 0.4F * fy + 0.1F * fx * fy;
            }
        }
        shared::TerrainWorld const world{atlas};
        uint32_t const spacing = 65'536U / resolution;
        for (uint32_t const origin : {0U, 65'536U - spacing}) {
            for (auto const level :
                 {shared::TerrainLodLevel::L0, shared::TerrainLodLevel::L1, shared::TerrainLodLevel::L2}) {
                auto const artifact = level == shared::TerrainLodLevel::L0
                    ? shared::buildTerrainLod(world, level)
                    : shared::buildTerrainLod(world, level, origin, origin, spacing);
                for (uint32_t row = 0U; row <= 8U; ++row) {
                    for (uint32_t col = 0U; col <= 8U; ++col) {
                        uint32_t const x = (origin + col * spacing / 8U) % 65'536U;
                        uint32_t const y = (origin + row * spacing / 8U) % 65'536U;
                        auto const surface = world.sampleSurface({
                            .x = static_cast<int32_t>(x / 16U),
                            .y = static_cast<int32_t>(y / 16U),
                            .z = 0,
                        });
                        auto const& canonical = surface.columns[y % 16U * 16U + x % 16U];
                        auto const sample = shared::sampleTerrainLod(*artifact, x, y);
                        EXPECT_NEAR(sample.temperature, canonical.temperature_celsius, 1e-6);
                        EXPECT_NEAR(sample.precipitation, canonical.precipitation, 1e-6);
                        EXPECT_EQ(sample.top_material,
                            canonical.materialAt(canonical.highest_solid_z, generation::BLOCK_SEA_LEVEL));
                    }
                }
            }
        }
    }
}

TEST(TerrainLodTest, OwnerSnowBoundaryKeepsCanonicalClimateWhenCoarseLevelChanges)
{
    auto const atlas = generation::prepareAtlas({.seed = 42U, .resolution = 256U});
    shared::TerrainWorld const world{atlas};
    uint32_t const x = 32'631U;
    uint32_t const y = 48'477U;
    auto const surface = world.sampleSurface({
        .x = static_cast<int32_t>(x / 16U),
        .y = static_cast<int32_t>(y / 16U),
        .z = 0,
    });
    decltype(surface.columns)::size_type const column_index =
        static_cast<decltype(surface.columns)::size_type>(y % shared::Chunk::SIDE_LENGTH)
        * shared::Chunk::SIDE_LENGTH + x % shared::Chunk::SIDE_LENGTH;
    auto const& canonical = surface.columns.at(column_index);
    ASSERT_LT(canonical.temperature_celsius, -2.0);
    ASSERT_EQ(canonical.top_material, shared::Block::Snow);
    for (auto const level :
         {shared::TerrainLodLevel::L0, shared::TerrainLodLevel::L1, shared::TerrainLodLevel::L2}) {
        uint32_t const spacing = shared::terrainLodSpacing(256U, level);
        auto const artifact = level == shared::TerrainLodLevel::L0
            ? shared::buildTerrainLod(world, level)
            : shared::buildTerrainLod(world, level, x / spacing * spacing, y / spacing * spacing, spacing);
        auto const sample = shared::sampleTerrainLod(*artifact, x, y);
        EXPECT_NEAR(sample.temperature, canonical.temperature_celsius, 1e-6);
        EXPECT_NEAR(sample.precipitation, canonical.precipitation, 1e-6);
        EXPECT_EQ(sample.top_material, shared::Block::Snow);
    }
}

TEST(TerrainLodTest, OwnerSeabedRemainsCanonicalInsteadOfClampingToFloorAtlasLand)
{
    auto const atlas = generation::prepareAtlas({.seed = 42U, .resolution = 256U});
    shared::TerrainWorld const world{atlas};
    uint32_t const x = 10'135U;
    uint32_t const y = 46'563U;
    auto const surface = world.sampleSurface({.x = static_cast<int32_t>(x / 16U),
        .y = static_cast<int32_t>(y / 16U), .z = 0});
    uint16_t const canonical = surface.columns[y % 16U * 16U + x % 16U].highest_solid_z;
    ASSERT_EQ(canonical, 214U);
    auto const l2 = shared::buildTerrainLod(world, shared::TerrainLodLevel::L2, 9'984U, 46'336U, 256U);
    EXPECT_NEAR(shared::sampleTerrainLod(*l2, x, y).height, canonical, 2.0);
}

TEST(TerrainLodTest, CoarseNodesSampleCanonicalR6SurfaceAtBothAtlasResolutions)
{
    for (uint32_t const resolution : {256U, 512U}) {
        auto const atlas = generation::prepareAtlas({.seed = 42U, .resolution = resolution});
        shared::TerrainWorld const world{atlas};
        for (auto const level :
             {shared::TerrainLodLevel::L0, shared::TerrainLodLevel::L1, shared::TerrainLodLevel::L2}) {
            auto const artifact = level == shared::TerrainLodLevel::L0
                ? shared::buildTerrainLod(world, level)
                : shared::buildTerrainLod(world, level, 9'984U, 46'336U, 256U);
            uint32_t const spacing = artifact->spacing;
            for (auto const point : {shared::TerrainLodNodeKey{9'984U / spacing * spacing, 46'336U / spacing * spacing},
                     shared::TerrainLodNodeKey{10'112U / spacing * spacing, 46'464U / spacing * spacing}}) {
                auto const surface = world.sampleSurface({.x = static_cast<int32_t>(point.x / 16U),
                    .y = static_cast<int32_t>(point.y / 16U), .z = 0});
                auto const& column = surface.columns[point.y % 16U * 16U + point.x % 16U];
                EXPECT_DOUBLE_EQ(shared::sampleTerrainLod(*artifact, point.x, point.y).height,
                    column.highest_solid_z) << "resolution=" << resolution << " level=" << static_cast<int>(level);
            }
        }
    }
}

TEST(TerrainLodTest, CanonicalPointHeightMatchesActualSurfaceAcrossPeriodicBlockCoordinates)
{
    auto const atlas = generation::prepareAtlas({.seed = 42U, .resolution = 256U});
    shared::TerrainWorld const world{atlas};
    for (auto const point : {shared::TerrainLodNodeKey{0U, 0U}, shared::TerrainLodNodeKey{65'535U, 65'535U},
             shared::TerrainLodNodeKey{10'135U, 46'563U}, shared::TerrainLodNodeKey{60'490U, 6'508U}}) {
        auto const surface = world.sampleSurface({.x = static_cast<int32_t>(point.x / 16U),
            .y = static_cast<int32_t>(point.y / 16U), .z = 0});
        auto const& column = surface.columns[
            static_cast<std::vector<float>::size_type>(point.y % 16U) * 16U + point.x % 16U
        ];
        EXPECT_EQ(world.sampleSurfaceHeight(point.x, point.y), column.highest_solid_z);
        EXPECT_EQ(world.sampleSurfaceHeight(point.x + 65'536U, point.y + 65'536U), column.highest_solid_z);
    }
}

TEST(TerrainLodTest, InterpolatedClimateRetainsCanonicalPrecisionAtTheSnowBoundary)
{
    static constexpr double SNOW_BOUNDARY = -2.0;
    static constexpr float TEMPERATURE = -2.0F;
    static constexpr float PRECIPITATION = 1.0F;
    static constexpr uint32_t PERIOD = generation::WORLD_SIZE_BLOCKS;

    for (uint32_t const resolution : {256U, 512U}) {
        auto atlas = std::make_shared<generation::GlobalAtlas>(*makeLodWorld(resolution)->atlas());
        std::vector<float>::size_type const sample_count =
            static_cast<std::vector<float>::size_type>(resolution) * resolution;
        atlas->temperature.assign(sample_count, TEMPERATURE);
        atlas->temperature[1U] = std::nextafter(TEMPERATURE, 0.0F);
        atlas->precipitation.assign(sample_count, PRECIPITATION);
        atlas->precipitation[1U] = std::nextafter(PRECIPITATION, 2.0F);
        shared::TerrainWorld const world{atlas};
        uint32_t const x = PERIOD / resolution / 16U;
        auto const surface = world.sampleSurface({
            .x = static_cast<int32_t>(x / shared::Chunk::SIDE_LENGTH), .y = 0, .z = 0,
        });
        auto const& canonical = surface.columns[x % shared::Chunk::SIDE_LENGTH];
        ASSERT_GT(canonical.temperature_celsius, SNOW_BOUNDARY);
        ASSERT_EQ(static_cast<float>(canonical.temperature_celsius), TEMPERATURE);
        ASSERT_EQ(canonical.top_material, shared::Block::Grass);
        for (auto const level : {shared::TerrainLodLevel::L0, shared::TerrainLodLevel::L1,
                 shared::TerrainLodLevel::L2, shared::TerrainLodLevel::L3}) {
            uint32_t const spacing = shared::terrainLodSpacing(resolution, level);
            auto const artifact = level == shared::TerrainLodLevel::L0
                ? shared::buildTerrainLod(world, level)
                : shared::buildTerrainLod(world, level, x / spacing * spacing, 0U, spacing);
            auto const sample = shared::sampleTerrainLod(*artifact, x, 0.0);
            EXPECT_DOUBLE_EQ(sample.temperature, canonical.temperature_celsius);
            EXPECT_DOUBLE_EQ(sample.precipitation, canonical.precipitation);
            EXPECT_EQ(sample.top_material, shared::Block::Grass);
            if (x % spacing == 0U) {
                EXPECT_DOUBLE_EQ(artifact->nodes.front().sample.temperature, canonical.temperature_celsius);
                EXPECT_EQ(artifact->nodes.front().sample.top_material, shared::Block::Grass);
            }
        }
    }
}
