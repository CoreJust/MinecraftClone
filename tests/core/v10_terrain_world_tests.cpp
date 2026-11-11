#include "V10ProviderInternal.hpp"

#include <server/HeightTileWorkerPool.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>
#include <generation/RiverBedChannel.hpp>
#include <generation/V10Provider.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

class V10TerrainWorldTests : public testing::Test {
protected:
    static std::shared_ptr<generation::GlobalAtlas const> atlas(bool const use_fine_terrain = false)
    {
        if (use_fine_terrain) {
            static auto const fine_prepared = [] {
                generation::GenerationConfiguration configuration;
                configuration.model_tag = generation::TerrainModelTag::V10;
                configuration.use_v10_fine_terrain = true;
                return generation::prepareAtlas(configuration);
            }();
            return fine_prepared;
        }
        static auto const prepared = [] {
            generation::GenerationConfiguration configuration;
            configuration.model_tag = generation::TerrainModelTag::V10;
            return generation::prepareAtlas(configuration);
        }();
        return prepared;
    }
};

struct CoastalProfile final {
    uint32_t dry_x = 0U;
    uint32_t shallow_x = 0U;
    uint32_t deep_x = 0U;
    uint32_t y = 0U;
    generation::V10SurfaceSample dry;
    generation::V10SurfaceSample shallow;
    generation::V10SurfaceSample deep;
};

std::optional<CoastalProfile> findCoastalProfile(generation::GlobalAtlas const& atlas)
{
    std::span<double const> elevation;
    std::span<uint8_t const> ocean;
    std::span<uint8_t const> lake;
    std::span<uint8_t const> river;
    for (generation::V9RegisteredFieldView const& field : generation::v10RegisteredFields(atlas)) {
        if (field.name == "height_m") {
            if (auto const values = std::get_if<std::span<double const>>(&field.values)) {
                elevation = *values;
            }
        } else if (field.name == "ocean" || field.name == "lake" || field.name == "river") {
            auto const values = std::get_if<std::span<uint8_t const>>(&field.values);
            if (values == nullptr) {
                continue;
            }
            if (field.name == "ocean") {
                ocean = *values;
            } else if (field.name == "lake") {
                lake = *values;
            } else {
                river = *values;
            }
        }
    }
    if (elevation.size() != 256U * 256U || ocean.size() != elevation.size()
        || lake.size() != elevation.size() || river.size() != elevation.size()) {
        return std::nullopt;
    }

    constexpr uint32_t SIDE = 256U;
    constexpr uint32_t CELL_SPACING = 256U;
    constexpr uint32_t WORLD_SIZE = SIDE * CELL_SPACING;
    constexpr double COAST_SAND_MAX_ELEVATION_M = 20.0;
    auto const is_low = [=](double const value) {
        return value > 0.0 && value <= COAST_SAND_MAX_ELEVATION_M;
    };
    auto const has_adjacent_water = [&atlas](uint32_t const x, uint32_t const y) {
        for (auto const& offset : std::array<std::array<double, 2U>, 4U>{{
                 {{-1.0, 0.0}}, {{1.0, 0.0}}, {{0.0, -1.0}}, {{0.0, 1.0}},
             }}) {
            generation::V10SurfaceSample const neighbor = generation::sampleV10AnnualMeanSurface(
                atlas, static_cast<double>(x) + offset[0], static_cast<double>(y) + offset[1], 3U);
            if (neighbor.ocean || neighbor.lake) {
                return true;
            }
        }
        return false;
    };

    struct Candidate final {
        uint32_t dry_x = 0U;
        uint32_t y = 0U;
        int32_t direction = 0;
        uint32_t deep_steps = 0U;
        double score = 0.0;
    };
    std::vector<Candidate> candidates;
    for (uint32_t y = 0U; y < SIDE; ++y) {
        for (uint32_t x = 0U; x < SIDE; ++x) {
            size_t const dry_index = static_cast<size_t>(y) * SIDE + x;
            if (ocean[dry_index] != 0U || lake[dry_index] != 0U || river[dry_index] != 0U
                || !is_low(elevation[dry_index])) {
                continue;
            }
            for (int32_t direction : {-1, 1}) {
                uint32_t const edge_x = static_cast<uint32_t>(
                    (static_cast<int32_t>(x) + direction + static_cast<int32_t>(SIDE))
                    % static_cast<int32_t>(SIDE));
                size_t const edge_index = static_cast<size_t>(y) * SIDE + edge_x;
                if (ocean[edge_index] == 0U || elevation[edge_index] >= 0.0) {
                    continue;
                }
                uint32_t deep_steps = 0U;
                for (uint32_t step = 1U; step < SIDE; ++step) {
                    uint32_t const deep_x = static_cast<uint32_t>(
                        (static_cast<int32_t>(x) + static_cast<int32_t>(step) * direction
                            + static_cast<int32_t>(SIDE) * static_cast<int32_t>(SIDE))
                        % static_cast<int32_t>(SIDE));
                    size_t const deep_index = static_cast<size_t>(y) * SIDE + deep_x;
                    if (ocean[deep_index] == 0U || lake[deep_index] != 0U || river[deep_index] != 0U) {
                        break;
                    }
                    if (elevation[deep_index] < -COAST_SAND_MAX_ELEVATION_M) {
                        deep_steps = step;
                        break;
                    }
                }
                if (deep_steps != 0U) {
                    candidates.push_back({
                        .dry_x = x * CELL_SPACING,
                        .y = y * CELL_SPACING,
                        .direction = direction,
                        .deep_steps = deep_steps,
                        .score = std::abs(elevation[dry_index]) + std::abs(elevation[edge_index]),
                    });
                }
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](Candidate const& left, Candidate const& right) {
        if (left.score != right.score) {
            return left.score < right.score;
        }
        if (left.deep_steps != right.deep_steps) {
            return left.deep_steps < right.deep_steps;
        }
        if (left.y != right.y) {
            return left.y < right.y;
        }
        if (left.dry_x != right.dry_x) {
            return left.dry_x < right.dry_x;
        }
        return left.direction < right.direction;
    });

    auto const wrap_x = [](int64_t const x) {
        return static_cast<uint32_t>((x % WORLD_SIZE + WORLD_SIZE) % WORLD_SIZE);
    };
    for (Candidate const& candidate : candidates) {
        uint32_t const dry_x = candidate.dry_x;
        uint32_t const y = candidate.y;
        generation::V10SurfaceSample const dry_endpoint = generation::sampleV10AnnualMeanSurface(
            atlas, dry_x, y, 3U);
        uint32_t const edge_x = wrap_x(static_cast<int64_t>(dry_x)
            + static_cast<int64_t>(candidate.direction) * CELL_SPACING);
        generation::V10SurfaceSample const wet_endpoint = generation::sampleV10AnnualMeanSurface(
            atlas, edge_x, y, 3U);
        uint32_t const deep_x = wrap_x(static_cast<int64_t>(dry_x)
            + static_cast<int64_t>(candidate.direction) * candidate.deep_steps * CELL_SPACING);
        generation::V10SurfaceSample const deep = generation::sampleV10AnnualMeanSurface(
            atlas, deep_x, y, 3U);
        if (dry_endpoint.ocean || dry_endpoint.lake || dry_endpoint.river || !is_low(dry_endpoint.elevation_m)
            || !wet_endpoint.ocean || wet_endpoint.elevation_m >= 0.0
            || !deep.ocean || deep.water_level_m - deep.elevation_m <= COAST_SAND_MAX_ELEVATION_M) {
            continue;
        }

        std::vector<generation::V10SurfaceSample> const shoreline = [&] {
            std::vector<generation::V10SurfaceSample> samples;
            samples.reserve(CELL_SPACING + 1U);
            for (uint32_t step = 0U; step <= CELL_SPACING; ++step) {
                if (step == 0U) {
                    samples.push_back(dry_endpoint);
                } else if (step == CELL_SPACING) {
                    samples.push_back(wet_endpoint);
                } else {
                    uint32_t const sample_x = wrap_x(static_cast<int64_t>(dry_x)
                        + static_cast<int64_t>(candidate.direction) * step);
                    samples.push_back(generation::sampleV10AnnualMeanSurface(atlas, sample_x, y, 3U));
                }
            }
            return samples;
        }();

        uint32_t dry_step = CELL_SPACING + 1U;
        for (uint32_t step = 0U; step < CELL_SPACING; ++step) {
            generation::V10SurfaceSample const& sample = shoreline[step];
            if (sample.ocean || sample.lake || sample.river || !is_low(sample.elevation_m)
                || generation::classifyV10AnnualMeanSurfaceCover(sample)
                    != generation::V10SurfaceCover::None
                || !shoreline[step + 1U].ocean) {
                continue;
            }
            uint32_t const sample_x = wrap_x(static_cast<int64_t>(dry_x)
                + static_cast<int64_t>(candidate.direction) * step);
            if (has_adjacent_water(sample_x, y)) {
                dry_step = step;
                break;
            }
        }
        if (dry_step > CELL_SPACING) {
            continue;
        }

        for (uint32_t step = dry_step + 1U; step <= CELL_SPACING; ++step) {
            generation::V10SurfaceSample const& shallow = shoreline[step];
            double const depth = shallow.water_level_m - shallow.elevation_m;
            if (!shallow.ocean || shallow.elevation_m < -COAST_SAND_MAX_ELEVATION_M
                || shallow.elevation_m >= 0.0 || !(depth > 0.0)
                || depth > COAST_SAND_MAX_ELEVATION_M
                || static_cast<uint64_t>(candidate.deep_steps) * CELL_SPACING <= step) {
                continue;
            }
            uint32_t const shallow_x = wrap_x(static_cast<int64_t>(dry_x)
                + static_cast<int64_t>(candidate.direction) * step);
            if (!has_adjacent_water(shallow_x, y)) {
                continue;
            }
            return CoastalProfile{
                .dry_x = wrap_x(static_cast<int64_t>(dry_x)
                    + static_cast<int64_t>(candidate.direction) * dry_step),
                .shallow_x = shallow_x,
                .deep_x = deep_x,
                .y = y,
                .dry = shoreline[dry_step],
                .shallow = shallow,
                .deep = deep,
            };
        }
    }
    return std::nullopt;
}

TEST_F(V10TerrainWorldTests, CanonicalIdentityBindsTheRiverComponent)
{
    generation::GenerationConfiguration configuration;
    configuration.model_tag = generation::TerrainModelTag::V10;
    shared::TerrainIdentity identity{
        .terrain_version = generation::terrainVersion(generation::TerrainModelTag::V10),
        .revision = 1U,
        .seed = configuration.seed,
        .configuration_identity = generation::configurationIdentity(configuration),
    };
    ASSERT_TRUE(shared::isValidTerrainIdentity(identity));
    size_t const component = identity.configuration_identity.find(";river=");
    ASSERT_NE(component, std::string::npos);
    auto const canonical = identity.configuration_identity;
    identity.configuration_identity.back() = identity.configuration_identity.back() == '0' ? '1' : '0';
    EXPECT_FALSE(shared::isValidTerrainIdentity(identity));
    identity.configuration_identity = canonical.substr(0U, component);
    EXPECT_FALSE(shared::isValidTerrainIdentity(identity));
}

TEST_F(V10TerrainWorldTests, RealProviderColumnsRetainOneMetreMountainsAndLayeredSolids)
{
    shared::TerrainWorld const world{atlas()};
    EXPECT_EQ(world.identity().terrain_version, 10U);
    EXPECT_TRUE(shared::isValidTerrainIdentity(world.identity()));
    EXPECT_NE(world.identity().configuration_identity.find("scale=1;datum=512;depth=2048"), std::string::npos);
    EXPECT_EQ(world.sampleSurfaceHeight(46'336U, 9'984U), 1'235U);
    EXPECT_EQ(world.sampleSurfaceHeight(46'357U, 10'018U), 1'238U);
    EXPECT_EQ(world.sampleSurfaceHeight(46'357U + generation::WORLD_SIZE_BLOCKS, 10'018U), 1'238U);

    auto const tile = world.sampleSurface({.x = 46'357 / 16, .y = 10'018 / 16, .z = 0});
    ASSERT_TRUE(shared::isValidSurfaceTile(tile));
    EXPECT_EQ(tile.block_sea_level, 512U);
    EXPECT_EQ(tile.max_height, 2'047U);
    auto const& column = tile.columns[(10'018U % 16U) * 16U + 46'357U % 16U];
    auto const source = generation::sampleV10AnnualMeanSurface(*atlas(), 46'357.0, 10'018.0, 3U);
    EXPECT_NEAR(source.elevation_m, 726.704'491'978'525'8, 1e-8);
    EXPECT_EQ(column.highest_solid_z, 1'238U);
    EXPECT_DOUBLE_EQ(column.source_l0_elevation_m, source.elevation_m);
    EXPECT_EQ(column.source_biome_id, source.biome_id);
    EXPECT_DOUBLE_EQ(column.temperature_celsius, source.temperature_celsius);
    EXPECT_DOUBLE_EQ(column.precipitation, source.precipitation_mm_year);
    EXPECT_DOUBLE_EQ(column.geological_elevation_km, source.elevation_m / 1'000.0);
    for (uint16_t z = 0U; z <= column.highest_solid_z; ++z) {
        EXPECT_TRUE(shared::isSolidBlock(column.materialAt(z, tile.block_sea_level))) << z;
    }
    EXPECT_EQ(column.materialAt(1'239U, tile.block_sea_level), shared::Block::Air);
    auto const climate = world.sampleClimate(46'357.0 - generation::WORLD_SIZE_BLOCKS, 10'018.0);
    EXPECT_DOUBLE_EQ(climate.temperature_celsius, source.temperature_celsius);
    EXPECT_DOUBLE_EQ(climate.precipitation, source.precipitation_mm_year);
}

TEST_F(V10TerrainWorldTests, ExplicitFineCandidateFeedsAllCanonicalQueriesAndPreservesWaterBeds)
{
    constexpr uint32_t X = 46'336U;
    constexpr uint32_t Y = 9'984U;
    auto const baseline_atlas = atlas();
    auto const fine_atlas = atlas(true);
    shared::TerrainWorld const baseline{baseline_atlas};
    shared::TerrainWorld const fine{fine_atlas};
    EXPECT_NE(baseline.identity().configuration_identity, fine.identity().configuration_identity);
    EXPECT_EQ(fine.identity().configuration_identity,
        generation::configurationIdentity(fine_atlas->configuration));
    EXPECT_TRUE(fine_atlas->configuration.use_v10_fine_terrain);

    auto const baseline_ridge = generation::sampleV10AnnualMeanSurface(*baseline_atlas, X, Y, 3U);
    auto const fine_reference = generation::sampleV10FineTerrainSurface(*fine_atlas, X, Y, 3U);
    for (uint32_t lod = 0U; lod < 4U; ++lod) {
        auto const sample = generation::sampleV10AnnualMeanSurface(*fine_atlas, X, Y, lod);
        EXPECT_DOUBLE_EQ(sample.elevation_m, fine_reference.elevation_m);
        EXPECT_EQ(sample.water_level_m, fine_reference.water_level_m);
        EXPECT_EQ(sample.ocean, fine_reference.ocean);
        EXPECT_EQ(sample.lake, fine_reference.lake);
        EXPECT_EQ(sample.river, fine_reference.river);
    }
    EXPECT_DOUBLE_EQ(generation::v10L0ElevationAt(*fine_atlas, X, Y), fine_reference.elevation_m);
    EXPECT_DOUBLE_EQ(generation::sampleV10AnnualMeanSurface(*baseline_atlas, X, Y, 3U).elevation_m,
        baseline_ridge.elevation_m);

    auto const water_support = [](
        generation::GlobalAtlas const& source,
        generation::V10SurfaceSample const& sample,
        std::array<double, 2U> const xy) {
        std::array<std::array<double, 2U>, 1U> const points{xy};
        auto const native = generation::queryV10NativeBedSamples(source, points);
        auto const river_field = generation::v10RiverBedField(source);
        river_field_candidate::NativePointSample const input{
            {xy[0], xy[1]}, native.front().foundation_m,
            native.front().standing_stage_m, native.front().standing_owner};
        auto const channels = river_field_candidate::sampleRiverChannel(*river_field, std::span{&input, 1U});
        if (!channels) {
            throw std::runtime_error(channels.error().message);
        }
        return generation::detail::queryV10WaterPoint(source.v10_provider->water,
            xy[0], xy[1], sample.elevation_m, channels->front().containing_courses);
    };
    for (std::array<double, 2U> const xy : {
             std::array<double, 2U>{34'816.0, 30'208.0},
             std::array<double, 2U>{43'520.0, 9'216.0},
         }) {
        auto const before = generation::sampleV10AnnualMeanSurface(*baseline_atlas, xy[0], xy[1], 3U);
        auto const after = generation::sampleV10AnnualMeanSurface(*fine_atlas, xy[0], xy[1], 3U);
        EXPECT_DOUBLE_EQ(after.elevation_m, before.elevation_m);
        EXPECT_DOUBLE_EQ(after.water_level_m, before.water_level_m);
        EXPECT_EQ(after.ocean, before.ocean);
        EXPECT_EQ(after.lake, before.lake);
        EXPECT_EQ(after.river, before.river);
        auto const before_water = water_support(*baseline_atlas, before, xy);
        auto const after_water = water_support(*fine_atlas, after, xy);
        ASSERT_TRUE(before_water);
        ASSERT_TRUE(after_water);
        EXPECT_EQ(after_water->river_owner_id, before_water->river_owner_id);
        EXPECT_EQ(after_water->lake_id, before_water->lake_id);
    }

    uint16_t const expected_height = generation::mapV10ElevationToBlock(fine_reference.elevation_m);
    EXPECT_EQ(fine.sampleSurfaceHeight(X, Y), expected_height);
    auto const tile = fine.sampleSurface({.x = static_cast<int32_t>(X / 16U),
        .y = static_cast<int32_t>(Y / 16U), .z = 0});
    ASSERT_TRUE(shared::isValidSurfaceTile(tile));
    auto const& column = tile.columns[(Y % 16U) * 16U + X % 16U];
    EXPECT_EQ(column.highest_solid_z, expected_height);
    EXPECT_DOUBLE_EQ(column.source_l0_elevation_m, fine_reference.elevation_m);
    auto const surface_chunk = fine.materializeChunk(tile,
        {.x = static_cast<int32_t>(X / 16U), .y = static_cast<int32_t>(Y / 16U),
            .z = static_cast<int32_t>(expected_height / 16U)});
    EXPECT_EQ(surface_chunk.blockAt({static_cast<uint8_t>(X % 16U), static_cast<uint8_t>(Y % 16U),
        static_cast<uint8_t>(expected_height % 16U)}),
        column.materialAt(expected_height, tile.block_sea_level));

    auto const lod = shared::buildTerrainLod(fine, shared::TerrainLodLevel::L3, X, Y, 64U);
    auto const mesh_sample = shared::sampleTerrainLod(*lod, X, Y);
    EXPECT_EQ(mesh_sample.height, expected_height);
    EXPECT_DOUBLE_EQ(mesh_sample.source_l0_elevation_m, fine_reference.elevation_m);
}

TEST_F(V10TerrainWorldTests, EveryLodVertexUsesTheCanonicalPointAndDecodedArtifactsInterpolate)
{
    shared::TerrainWorld const world{atlas()};
    for (auto const level : {shared::TerrainLodLevel::L0, shared::TerrainLodLevel::L1,
             shared::TerrainLodLevel::L2, shared::TerrainLodLevel::L3}) {
        auto const artifact = level == shared::TerrainLodLevel::L0
            ? shared::buildTerrainLod(world, level)
            : shared::buildTerrainLod(world, level, 46'336U, 9'984U, 64U);
        EXPECT_EQ(artifact->model_identity, shared::TERRAIN_LOD_MODEL_V10);
        double const sample_x = level == shared::TerrainLodLevel::L3 ? 46'357.0 : 46'336.0;
        double const sample_y = level == shared::TerrainLodLevel::L3 ? 10'018.0 : 9'984.0;
        auto const point = shared::sampleTerrainLod(*artifact, sample_x, sample_y);
        auto const wrapped = shared::sampleTerrainLod(*artifact,
            sample_x - generation::WORLD_SIZE_BLOCKS, sample_y + generation::WORLD_SIZE_BLOCKS);
        EXPECT_EQ(point, wrapped);
        EXPECT_EQ(point.height, level == shared::TerrainLodLevel::L3 ? 1'238.0 : 1'235.0);
        EXPECT_EQ(point.parent_height, point.height);
        EXPECT_TRUE(point.v10_surface);
        EXPECT_FALSE(point.v21_surface);
        auto const source = generation::sampleV10AnnualMeanSurface(*atlas(), sample_x, sample_y, 3U);
        EXPECT_DOUBLE_EQ(point.source_l0_elevation_m, source.elevation_m);
        EXPECT_EQ(point.source_native_ocean, source.ocean);
        EXPECT_DOUBLE_EQ(point.temperature, source.temperature_celsius);
        EXPECT_DOUBLE_EQ(point.precipitation, source.precipitation_mm_year);
        EXPECT_EQ(point.source_biome_id, source.biome_id);
        EXPECT_DOUBLE_EQ(point.land_snow_water_equivalent_m, source.snow_water_equivalent_m);
        EXPECT_DOUBLE_EQ(point.glacier_thickness_m, source.glacier_thickness_m);
        EXPECT_DOUBLE_EQ(point.sea_ice_thickness_m, source.sea_ice_thickness_m);
        EXPECT_EQ(point.water_present, source.ocean || source.lake || source.river);
        EXPECT_EQ(point.water_height, generation::mapV10ElevationToBlock(source.water_level_m));
        auto const block_x = static_cast<uint32_t>(sample_x);
        auto const block_y = static_cast<uint32_t>(sample_y);
        auto const tile = world.sampleSurface({.x = static_cast<int32_t>(block_x / 16U),
            .y = static_cast<int32_t>(block_y / 16U), .z = 0});
        auto const& column = tile.columns[(block_y % 16U) * 16U + block_x % 16U];
        EXPECT_EQ(point.top_material, column.materialAt(column.highest_solid_z, 512U));
        shared::TerrainLodArtifact const decoded_copy = *artifact;
        EXPECT_EQ(shared::sampleTerrainLod(decoded_copy, sample_x, sample_y), point);
        double const midpoint_x = artifact->origin_x + artifact->spacing / 2.0;
        double const midpoint_y = artifact->origin_y + artifact->spacing / 2.0;
        auto const midpoint = shared::sampleTerrainLod(decoded_copy, midpoint_x, midpoint_y);
        double const expected_interpolated_height = (artifact->nodes[0].sample.height
            + artifact->nodes[1].sample.height + artifact->nodes[artifact->side].sample.height
            + artifact->nodes[artifact->side + 1U].sample.height) / 4.0;
        EXPECT_DOUBLE_EQ(midpoint.height, expected_interpolated_height);
        for (size_t const index : std::array<size_t, 4>{0U, 1U, artifact->nodes.size() / 2U,
                 artifact->nodes.size() - 1U}) {
            auto const& node = artifact->nodes[index];
            EXPECT_EQ(node.sample.height, world.sampleSurfaceHeight(node.key.x, node.key.y));
            EXPECT_EQ(node.sample.parent_height, node.sample.height);
        }
    }
}

TEST_F(V10TerrainWorldTests, PositiveColdLakeBedUsesDirtWithoutChangingWaterOrSolidHeight)
{
    static constexpr uint32_t X = 43'520U;
    static constexpr uint32_t Y = 9'216U;
    auto const world = std::make_shared<shared::TerrainWorld const>(atlas());
    auto const source = generation::sampleV10AnnualMeanSurface(*atlas(), X, Y, 3U);
    ASSERT_TRUE(source.lake);
    EXPECT_FALSE(source.ocean);
    EXPECT_FALSE(source.river);
    EXPECT_NEAR(source.elevation_m, 315.373'544, 1e-3);
    EXPECT_NEAR(source.water_level_m, 316.759, 1e-2);
    EXPECT_NEAR(source.temperature_celsius, -21.075'552, 1e-3);
    EXPECT_EQ(source.biome_id, 11U);
    EXPECT_DOUBLE_EQ(source.snow_water_equivalent_m, 0.0);
    EXPECT_DOUBLE_EQ(source.glacier_thickness_m, 0.0);

    auto const tile = world->sampleSurface({.x = static_cast<int32_t>(X / 16U),
        .y = static_cast<int32_t>(Y / 16U), .z = 0});
    ASSERT_TRUE(shared::isValidSurfaceTile(tile));
    auto const& column = tile.columns[(Y % 16U) * 16U + X % 16U];
    EXPECT_EQ(column.highest_solid_z, generation::mapV10ElevationToBlock(source.elevation_m));
    EXPECT_EQ(column.water_height_z, generation::mapV10ElevationToBlock(source.water_level_m));
    EXPECT_TRUE(column.water_present);
    EXPECT_FALSE(column.source_native_ocean);
    EXPECT_FALSE(column.submerged);
    EXPECT_DOUBLE_EQ(column.temperature_celsius, source.temperature_celsius);
    EXPECT_EQ(column.source_biome_id, 11U);
    EXPECT_EQ(column.top_material, shared::Block::Dirt);
    EXPECT_TRUE(shared::isValidTerrainColumn(column, tile.block_sea_level, tile.max_height));
    shared::TerrainLodSample lod_sample{
        .height = static_cast<double>(column.highest_solid_z),
        .water_present = column.water_present,
        .temperature = column.temperature_celsius,
        .source_l0_elevation_m = column.source_l0_elevation_m,
        .source_biome_id = column.source_biome_id,
        .v10_surface = true,
    };
    EXPECT_EQ(shared::terrainLodSurfaceMaterial(lod_sample, tile.block_sea_level), column.top_material);
    auto grass_column = column;
    grass_column.top_material = shared::Block::Grass;
    EXPECT_FALSE(shared::isValidTerrainColumn(grass_column, tile.block_sea_level, tile.max_height));
    EXPECT_EQ(column.materialAt(column.highest_solid_z, tile.block_sea_level), shared::Block::Dirt);
    EXPECT_EQ(column.materialAt(static_cast<uint16_t>(column.highest_solid_z + 1U), tile.block_sea_level),
        shared::Block::Air);
    EXPECT_EQ(column.materialAt(column.water_height_z, tile.block_sea_level), shared::Block::Air);

    shared::SparseWorld sparse(
        {.seed = world->identity().seed, .revision = world->identity().revision}, world
    );
    ASSERT_TRUE(sparse.publishAuthoritativeSurface(tile));
    auto const verified = sparse.verifiedSurfaceColumn(X, Y);
    ASSERT_TRUE(verified.has_value());
    EXPECT_EQ(verified->highest_solid_z, column.highest_solid_z);
    EXPECT_EQ(verified->material_at_highest_solid, shared::Block::Dirt);
}

TEST_F(V10TerrainWorldTests, CoastalSandContinuesIntoShallowOceanAndStopsAtDeepBed)
{
    auto const profile = findCoastalProfile(*atlas());
    ASSERT_TRUE(profile.has_value()) << "seed-42 V10 atlas has no dry-to-shallow-to-deep ocean row";
    EXPECT_GT(profile->dry.elevation_m, 0.0);
    EXPECT_LE(profile->dry.elevation_m, 20.0);
    EXPECT_LT(profile->shallow.elevation_m, 0.0);
    EXPECT_GT(profile->shallow.water_level_m, profile->shallow.elevation_m);
    EXPECT_LE(profile->shallow.water_level_m - profile->shallow.elevation_m, 20.0);
    EXPECT_GT(profile->deep.water_level_m - profile->deep.elevation_m, 20.0);
    auto const world = std::make_shared<shared::TerrainWorld const>(atlas());
    shared::SparseWorld sparse(
        {.seed = world->identity().seed, .revision = world->identity().revision}, world
    );
    std::array<std::pair<uint32_t, generation::V10SurfaceSample const*>, 3U> const points{{
        {profile->dry_x, &profile->dry},
        {profile->shallow_x, &profile->shallow},
        {profile->deep_x, &profile->deep},
    }};
    std::array<shared::Block, 3U> const expected_top_materials{
        shared::Block::Sand,
        shared::Block::Sand,
        shared::Block::Dirt,
    };
    for (size_t index = 0U; index < points.size(); ++index) {
        uint32_t const x = points[index].first;
        uint32_t const y = profile->y;
        generation::V10SurfaceSample const& source = *points[index].second;
        auto const tile = world->sampleSurface({
            .x = static_cast<int32_t>(x / shared::Chunk::SIDE_LENGTH),
            .y = static_cast<int32_t>(y / shared::Chunk::SIDE_LENGTH),
            .z = 0,
        });
        ASSERT_TRUE(shared::isValidSurfaceTile(tile));
        shared::TerrainColumn const& column = tile.columns[(y % 16U) * 16U + x % 16U];
        EXPECT_EQ(column.highest_solid_z, generation::mapV10ElevationToBlock(source.elevation_m));
        EXPECT_EQ(column.water_height_z, generation::mapV10ElevationToBlock(source.water_level_m));
        EXPECT_EQ(column.water_present, source.ocean || source.lake || source.river);
        EXPECT_EQ(column.source_native_ocean, source.ocean);
        if (index < 2U) {
            EXPECT_TRUE(column.source_native_coast_adjacent);
        }
        EXPECT_EQ(column.top_material, expected_top_materials[index]);
        shared::Block const material_at_height = column.materialAt(column.highest_solid_z, tile.block_sea_level);
        if (index < 2U) {
            EXPECT_EQ(material_at_height, shared::Block::Sand);
        } else {
            EXPECT_NE(material_at_height, shared::Block::Sand);
        }
        EXPECT_TRUE(shared::isValidTerrainColumn(column, tile.block_sea_level, tile.max_height));
        shared::Block subsurface_material = shared::Block::Air;
        if (column.highest_solid_z > 1U) {
            subsurface_material = column.materialAt(
                static_cast<uint16_t>(column.highest_solid_z - 1U), tile.block_sea_level);
            EXPECT_NE(subsurface_material, shared::Block::Air);
        }
        EXPECT_EQ(world->sampleSurfaceHeight(x, y), column.highest_solid_z);
        EXPECT_TRUE(sparse.publishAuthoritativeSurface(tile));
        auto const verified = sparse.verifiedSurfaceColumn(x, y);
        ASSERT_TRUE(verified.has_value());
        EXPECT_EQ(verified->highest_solid_z, column.highest_solid_z);
        EXPECT_EQ(verified->material_at_highest_solid, material_at_height);
        if (column.highest_solid_z > 1U) {
            auto const subsurface = sparse.blockAt({
                .x = x,
                .y = y,
                .z = static_cast<int64_t>(column.highest_solid_z - 1U),
            });
            ASSERT_TRUE(subsurface.has_value());
            EXPECT_EQ(*subsurface, subsurface_material);
        }

        uint32_t const lod_origin_x = x / 64U * 64U;
        uint32_t const lod_origin_y = y / 64U * 64U;
        auto const lod = shared::buildTerrainLod(
            *world, shared::TerrainLodLevel::L3, lod_origin_x, lod_origin_y, 64U);
        auto const lod_sample = shared::sampleTerrainLod(*lod, x, y);
        EXPECT_EQ(lod_sample.height, column.highest_solid_z);
        EXPECT_EQ(lod_sample.top_material, material_at_height);
        EXPECT_EQ(lod_sample.source_native_ocean, source.ocean);
    }
}

TEST_F(V10TerrainWorldTests, ThinColdLandSnowUsesCanonicalSurfaceMaterialWithoutChangingHeight)
{
    static constexpr uint32_t X = 35'840U;
    static constexpr uint32_t Y = 5'120U;
    auto const world = std::make_shared<shared::TerrainWorld const>(atlas());
    auto const source = generation::sampleV10AnnualMeanSurface(*atlas(), X, Y, 3U);
    ASSERT_FALSE(source.ocean);
    ASSERT_FALSE(source.lake);
    ASSERT_FALSE(source.river);
    EXPECT_EQ(source.biome_id, 3U);
    EXPECT_NEAR(source.temperature_celsius, -8.846, 1e-3);
    EXPECT_NEAR(source.snow_water_equivalent_m, 0.018'859, 1e-6);
    EXPECT_NEAR(source.glacier_thickness_m, 0.005'096, 1e-6);
    EXPECT_EQ(generation::classifyV10AnnualMeanSurfaceCover(source), generation::V10SurfaceCover::Snow);

    auto const tile = world->sampleSurface({.x = static_cast<int32_t>(X / 16U),
        .y = static_cast<int32_t>(Y / 16U), .z = 0});
    ASSERT_TRUE(shared::isValidSurfaceTile(tile));
    auto const& column = tile.columns[(Y % 16U) * 16U + X % 16U];
    EXPECT_EQ(column.highest_solid_z, generation::mapV10ElevationToBlock(source.elevation_m));
    EXPECT_EQ(column.water_height_z, generation::mapV10ElevationToBlock(source.water_level_m));
    EXPECT_FALSE(column.water_present);
    EXPECT_EQ(column.top_material, shared::Block::Snow);
    EXPECT_EQ(column.materialAt(column.highest_solid_z, tile.block_sea_level), shared::Block::Snow);
    EXPECT_EQ(column.materialAt(static_cast<uint16_t>(column.highest_solid_z + 1U), tile.block_sea_level),
        shared::Block::Air);
    EXPECT_TRUE(shared::isValidTerrainColumn(column, tile.block_sea_level, tile.max_height));

    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L3, X, Y, 16U);
    auto const lod_sample = shared::sampleTerrainLod(*lod, X, Y);
    EXPECT_EQ(lod_sample.height, column.highest_solid_z);
    EXPECT_EQ(lod_sample.top_material, column.top_material);
}

TEST_F(V10TerrainWorldTests, ColdSummitWithoutSnowStockKeepsDryV10GroundMaterial)
{
    shared::TerrainLodSample sample;
    sample.height = 600.0;
    sample.temperature = -10.0;
    sample.source_l0_elevation_m = 88.0;
    sample.source_biome_id = 0U; // Cold-summit biome
    sample.v10_surface = true;

    EXPECT_EQ(shared::terrainLodSurfaceMaterial(sample, generation::V10_BLOCK_SEA_LEVEL), shared::Block::Grass);
}

TEST_F(V10TerrainWorldTests, R2RiverWitnessCapturesCurrentNativeSurfaceAndLodSamples)
{
    static constexpr uint32_t X = 12'512U;
    static constexpr uint32_t Z = 35'008U;
    static constexpr double R2_FOUNDATION_ELEVATION_M = 170.312'876'402;
    static constexpr double R2_RIVERBED_ELEVATION_M = 161.740'050'920;
    static constexpr double R2_WATER_HEAD_M = 164.248'337'118;
    static constexpr double R2_WATER_DEPTH_M = 2.508'286'198;
    static constexpr int32_t EXPECTED_R2_BED_GAP_BLOCKS = 9;
    static constexpr std::array<shared::TerrainLodLevel, 4U> LOD_LEVELS{
        shared::TerrainLodLevel::L0,
        shared::TerrainLodLevel::L1,
        shared::TerrainLodLevel::L2,
        shared::TerrainLodLevel::L3,
    };
    auto const record_double = [](std::string const& key, double const value) {
        std::ostringstream formatted;
        formatted << std::setprecision(17) << value;
        testing::Test::RecordProperty(key, formatted.str());
    };

    auto const source = generation::sampleV10AnnualMeanSurface(*atlas(), X, Z, 3U);
    record_double("native_source_elevation_m", source.elevation_m);
    record_double("native_source_water_head_m", source.water_level_m);
    record_double("r2_foundation_elevation_m", R2_FOUNDATION_ELEVATION_M);
    record_double("r2_constructed_bed_elevation_m", R2_RIVERBED_ELEVATION_M);
    record_double("r2_water_head_m", R2_WATER_HEAD_M);
    record_double("r2_water_depth_m", R2_WATER_DEPTH_M);
    testing::Test::RecordProperty("r2_constructed_river", "true");
    RecordProperty("native_source_ocean", source.ocean ? "true" : "false");
    RecordProperty("native_source_lake", source.lake ? "true" : "false");
    RecordProperty("native_source_river", source.river ? "true" : "false");
    EXPECT_NEAR(source.elevation_m, R2_FOUNDATION_ELEVATION_M, 1e-8);
    EXPECT_NEAR(R2_WATER_HEAD_M - R2_RIVERBED_ELEVATION_M, R2_WATER_DEPTH_M, 1e-9);

    uint16_t const r2_bed_y = generation::mapV10ElevationToBlock(R2_RIVERBED_ELEVATION_M);
    uint16_t const r2_water_head_y = generation::mapV10ElevationToBlock(R2_WATER_HEAD_M);
    EXPECT_EQ(r2_bed_y, 673U);
    EXPECT_EQ(r2_water_head_y, 676U);

    shared::TerrainWorld const world{atlas()};
    auto const tile = world.sampleSurface({.x = static_cast<int32_t>(X / 16U),
        .y = static_cast<int32_t>(Z / 16U), .z = 0});
    ASSERT_TRUE(shared::isValidSurfaceTile(tile));
    auto const& column = tile.columns[(Z % 16U) * 16U + X % 16U];
    uint16_t const native_bed_y = generation::mapV10ElevationToBlock(source.elevation_m);
    int32_t const native_bed_gap_blocks = static_cast<int32_t>(native_bed_y)
        - static_cast<int32_t>(r2_bed_y);
    record_double("native_column_source_elevation_m", column.source_l0_elevation_m);
    RecordProperty("native_column_bed_y", std::to_string(native_bed_y));
    RecordProperty("r2_constructed_bed_y", std::to_string(r2_bed_y));
    RecordProperty("native_minus_r2_bed_blocks", std::to_string(native_bed_gap_blocks));
    RecordProperty("native_column_water_present", column.water_present ? "true" : "false");
    RecordProperty("native_column_water_head_y", std::to_string(column.water_height_z));
    RecordProperty("native_column_top_material", std::to_string(static_cast<uint8_t>(column.top_material)));
    RecordProperty("native_column_material_at_r2_head",
        std::to_string(static_cast<uint8_t>(column.materialAt(r2_water_head_y, tile.block_sea_level))));
    EXPECT_DOUBLE_EQ(column.source_l0_elevation_m, source.elevation_m);
    EXPECT_EQ(column.highest_solid_z, native_bed_y);
    EXPECT_EQ(column.water_height_z, generation::mapV10ElevationToBlock(source.water_level_m));
    EXPECT_EQ(column.water_present, source.ocean || source.lake || source.river);
    EXPECT_EQ(native_bed_gap_blocks, EXPECTED_R2_BED_GAP_BLOCKS);

    for (size_t level_index = 0U; level_index < LOD_LEVELS.size(); ++level_index) {
        auto const level = LOD_LEVELS[level_index];
        uint32_t const spacing = shared::terrainLodSpacing(
            atlas()->configuration.resolution, level, generation::TerrainModelTag::V10);
        std::shared_ptr<shared::TerrainLodArtifact const> artifact;
        if (level == shared::TerrainLodLevel::L0) {
            artifact = shared::buildTerrainLod(world, level);
        } else {
            uint32_t const origin_x = X / spacing * spacing;
            uint32_t const origin_z = Z / spacing * spacing;
            artifact = shared::buildTerrainLod(world, level, origin_x, origin_z, spacing);
        }
        auto const lod_sample = shared::sampleTerrainLod(*artifact, X, Z);
        std::string const prefix = "lod_l" + std::to_string(level_index);
        record_double(prefix + "_height_block", lod_sample.height);
        record_double(prefix + "_source_elevation_m", lod_sample.source_l0_elevation_m);
        record_double(prefix + "_water_head_block", lod_sample.water_height);
        testing::Test::RecordProperty(prefix + "_water_present",
            lod_sample.water_present ? "true" : "false");
        testing::Test::RecordProperty(prefix + "_top_material",
            std::to_string(static_cast<uint8_t>(lod_sample.top_material)));
        ASSERT_TRUE(lod_sample.v10_surface);
        EXPECT_TRUE(std::isfinite(lod_sample.source_l0_elevation_m));
        if (level >= shared::TerrainLodLevel::L2) {
            EXPECT_DOUBLE_EQ(lod_sample.source_l0_elevation_m, source.elevation_m);
            EXPECT_EQ(lod_sample.height, static_cast<double>(native_bed_y));
            EXPECT_EQ(lod_sample.water_present, column.water_present);
            EXPECT_EQ(lod_sample.water_height, column.water_height_z);
            EXPECT_EQ(lod_sample.top_material, column.materialAt(column.highest_solid_z, tile.block_sea_level));
        }
    }
}

TEST_F(V10TerrainWorldTests, NativeHeightWorkerAcceptsSchedulerJobsWithoutRegionalLeaves)
{
    constexpr uint64_t WORLD_EPOCH = 1U;
    auto const world = std::make_shared<shared::TerrainWorld>(atlas());
    shared::WorldGenerationCoordinator coordinator{32U, world->identity().revision, world->identity().seed};
    coordinator.setTerrainContext(world, WORLD_EPOCH);
    shared::GenerationJob job{
        .id = 1U,
        .coordinate = {.x = 46'357 / 16, .y = 10'018 / 16, .z = 1'238 / 16},
        .revision = world->identity().revision,
        .seed = world->identity().seed,
        .stage = shared::GenerationStage::HeightTile,
    };
    auto const bound = coordinator.bindTerrainLeaf(job);
    ASSERT_TRUE(bound.has_value());
    ASSERT_EQ(bound->inherited_region, nullptr);
    server::HeightTileWorkerPool pool{world, 1U};
    auto take = [&pool] {
        std::vector<server::HeightTileWorkerPool::Result> results;
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (results.empty() && std::chrono::steady_clock::now() < deadline) {
            results = pool.takeResults(1U);
            std::this_thread::yield();
        }
        return results;
    };
    ASSERT_TRUE(pool.enqueue({.client_id = 7U, .generation = job.revision, .job = *bound}));
    auto const results = take();
    ASSERT_EQ(results.size(), 1U);
    ASSERT_TRUE(results.front().succeeded);
    auto const reference = world->sampleSurface({job.coordinate.x, job.coordinate.y, 0});
    auto const& actual = results.front().tile.surface;
    ASSERT_EQ(actual.identity, reference.identity);
    for (size_t index = 0U; index < actual.columns.size(); ++index) {
        EXPECT_EQ(actual.columns[index].highest_solid_z, reference.columns[index].highest_solid_z);
        EXPECT_EQ(actual.columns[index].top_material, reference.columns[index].top_material);
    }
    auto materialization = *bound;
    materialization.id = 2U;
    materialization.stage = shared::GenerationStage::Materialize;
    ASSERT_TRUE(pool.enqueue({.client_id = 7U, .generation = job.revision,
        .job = materialization, .world_generation = true}));
    auto const dense = take();
    ASSERT_EQ(dense.size(), 1U);
    ASSERT_TRUE(dense.front().succeeded);
    EXPECT_EQ(dense.front().chunk.contentIdentity(),
        world->materializeChunk(reference, job.coordinate).contentIdentity());
    EXPECT_EQ(pool.queueMetrics().surface_tile_samples_total, 1U);
    EXPECT_EQ(pool.queueMetrics().surface_tile_cache_hits_total, 1U);
    auto stale = *bound;
    stale.world_epoch += 1U;
    EXPECT_FALSE(pool.enqueue({.client_id = 7U, .generation = job.revision, .job = stale}));
}

TEST_F(V10TerrainWorldTests, RegionalArtifactsUseTheSameCanonicalColumnAndRejectMixedModelFlags)
{
    shared::TerrainWorld const world{atlas()};
    auto const region = world.prepareRegion({.origin_x = 46'336U, .origin_y = 9'984U, .extent = 64U});
    auto const point = world.sampleRegion(*region, 46'357.0, 10'018.0);
    EXPECT_EQ(point.highest_solid_z, 1'238U);
    EXPECT_DOUBLE_EQ(point.source_l0_elevation_m,
        generation::sampleV10AnnualMeanSurface(*atlas(), 46'357.0, 10'018.0, 3U).elevation_m);
    shared::TerrainLodSample mixed;
    mixed.v10_surface = true;
    mixed.v21_surface = true;
    EXPECT_THROW((void)shared::terrainLodSurfaceMaterial(mixed, 512U), std::invalid_argument);
}

} // namespace
