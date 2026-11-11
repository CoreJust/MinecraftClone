#include <client/GenerationMapPresentation.hpp>

#include <client/S2MapPresentation.hpp>

#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <generation/Generation.hpp>
#include <generation/V9Provider.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{

using Rgba8 = client::GenerationMapRgba8;

[[nodiscard]]
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): maximum channel distance is symmetric.
uint32_t maximumChannelDistance(Rgba8 const& left, Rgba8 const& right) noexcept
{
    uint32_t distance = 0U;
    for (uint32_t channel = 0U; channel < 3U; ++channel)
    {
        uint32_t const left_value = left[channel];
        uint32_t const right_value = right[channel];
        distance = std::max(distance, left_value > right_value ? left_value - right_value : right_value - left_value);
    }
    return distance;
}

[[nodiscard]]
generation::GenerationSnapshot makeSnapshot(generation::GenerationStage const stage)
{
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr uint32_t FIELD_COUNT = RESOLUTION * RESOLUTION;

    generation::GenerationSnapshot snapshot;
    snapshot.stage = stage;
    snapshot.resolution = RESOLUTION;
    snapshot.elevation.assign(FIELD_COUNT, 0.5F);
    return snapshot;
}

void addHydrology(generation::GenerationSnapshot& snapshot)
{
    static constexpr uint32_t FIELD_COUNT = 256U * 256U;
    snapshot.water_level.assign(FIELD_COUNT, 0.0F);
    snapshot.ocean_mask.assign(FIELD_COUNT, 0U);
    snapshot.lake_mask.assign(FIELD_COUNT, 0U);
    snapshot.river_mask.assign(FIELD_COUNT, 0U);
}

void addClimate(generation::GenerationSnapshot& snapshot, float const temperature, float const precipitation)
{
    static constexpr uint32_t FIELD_COUNT = 256U * 256U;
    snapshot.temperature.assign(FIELD_COUNT, temperature);
    snapshot.precipitation.assign(FIELD_COUNT, precipitation);
}

[[nodiscard]]
uint32_t animationRenderPixelIndex(uint32_t const native_x, uint32_t const native_y) noexcept
{
    static constexpr uint32_t SCALE = client::GENERATION_MAP_ANIMATION_SCALE;
    static constexpr uint32_t RENDER_RESOLUTION = client::GENERATION_MAP_ANIMATION_RENDER_RESOLUTION;
    uint32_t const x = native_x * SCALE + SCALE / 2U;
    uint32_t const y = native_y * SCALE + SCALE / 2U;
    return y * RENDER_RESOLUTION + x;
}

[[nodiscard]]
std::shared_ptr<generation::GlobalAtlas const> makeDriftingMapAtlas()
{
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr uint32_t FIELD_COUNT = RESOLUTION * RESOLUTION;
    static constexpr uint32_t CENTER_CONTINENT_START = 128U;
    static constexpr uint32_t CENTER_CONTINENT_END = 192U;
    static constexpr uint32_t SEAM_CONTINENT_WIDTH = 8U;

    auto atlas = std::make_shared<generation::GlobalAtlas>();
    atlas->configuration.resolution = RESOLUTION;
    atlas->elevation.resize(FIELD_COUNT, -100.0F);
    atlas->plate_id.resize(FIELD_COUNT, 0);
    atlas->plate_vx.resize(FIELD_COUNT, 1.0F);
    atlas->plate_vy.resize(FIELD_COUNT, 0.0F);
    for (uint32_t y = 0U; y < RESOLUTION; ++y)
    {
        for (uint32_t x = 0U; x < RESOLUTION; ++x)
        {
            bool const center_continent = x >= CENTER_CONTINENT_START && x < CENTER_CONTINENT_END;
            bool const seam_continent = x < SEAM_CONTINENT_WIDTH || x >= RESOLUTION - SEAM_CONTINENT_WIDTH;
            if (center_continent || seam_continent)
            {
                atlas->elevation[y * RESOLUTION + x] = 100.0F;
            }
        }
    }
    return atlas;
}

[[nodiscard]]
double verticalEdgePosition(
    client::GenerationMapImage const& image,
    uint32_t const y,
    uint32_t const first_x,
    uint32_t const last_x
) noexcept
{
    double total_weight = 0.0;
    double weighted_position = 0.0;
    for (uint32_t x = first_x; x < last_x; ++x)
    {
        uint32_t const left = image.rgba[y * image.width + x][2U];
        uint32_t const right = image.rgba[y * image.width + x + 1U][2U];
        uint32_t const difference = left > right ? left - right : right - left;
        total_weight += static_cast<double>(difference);
        weighted_position += (static_cast<double>(x) + 0.5) * static_cast<double>(difference);
    }
    return total_weight > 0.0 ? weighted_position / total_weight : 0.0;
}

[[nodiscard]]
bool writePpmIfRequested(std::string const& filename, client::GenerationMapImage const& image)
{
    char const* const evidence_directory = std::getenv("MC_GENERATION_MAP_EVIDENCE_DIR");
    if (evidence_directory == nullptr || evidence_directory[0] == '\0')
    {
        return true;
    }

    std::ofstream output(std::filesystem::path(evidence_directory) / filename, std::ios::binary);
    if (!output)
    {
        return false;
    }
    output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (Rgba8 const& pixel : image.rgba)
    {
        output.write(reinterpret_cast<char const*>(pixel.data()), 3);
    }
    return output.good();
}

[[nodiscard]]
bool writeFrameTimingsIfRequested(
    std::array<uint64_t, 6U> const& elapsed_microseconds,
    std::array<uint8_t, 6U> const& phase_indices
)
{
    char const* const evidence_directory = std::getenv("MC_GENERATION_MAP_EVIDENCE_DIR");
    if (evidence_directory == nullptr || evidence_directory[0] == '\0')
    {
        return true;
    }

    std::ofstream output(std::filesystem::path(evidence_directory) / "frame-timings.csv");
    if (!output)
    {
        return false;
    }
    output << "frame,phase_index,update_microseconds\n";
    for (uint32_t frame = 0U; frame < elapsed_microseconds.size(); ++frame)
    {
        output << frame << ',' << static_cast<uint32_t>(phase_indices[frame]) << ','
               << elapsed_microseconds[frame] << '\n';
    }
    return output.good();
}

TEST(GenerationMapPresentationTest, NativeSharedRiverGeometryPreservesThermalStateAndEndpoint)
{
    generation::GenerationConfiguration configuration{};
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    auto const atlas = generation::prepareAtlas(configuration);
    shared::TerrainWorld const world(atlas, 17U);
    auto const lod = shared::buildTerrainLod(world, shared::TerrainLodLevel::L0);
    auto image = client::rasterizeS2WorldMap(world, *lod, {});
    ASSERT_TRUE(image);
    ASSERT_NE(image->v10_river_geometry, nullptr);
    auto source = std::make_shared<client::S2MapImage const>(std::move(*image));
    auto control = std::make_shared<client::S2MapImage>(*source);
    control->v10_river_geometry.reset();
    client::GenerationMapAnimator geometry_animator;
    client::GenerationMapAnimator control_animator;
    bool intermediate_difference = false;
    for (int64_t seconds : {0, 10, 18, 28, 40, 60})
    {
        client::GenerationMapAnimationClock clock;
        clock.visible_elapsed = std::chrono::seconds{seconds};
        auto const frame = geometry_animator.updateNative(atlas, source, clock);
        auto const reference = control_animator.updateNative(atlas, control, clock);
        ASSERT_TRUE(frame.surface_image);
        ASSERT_TRUE(reference.surface_image);
        EXPECT_EQ(frame.phase, reference.phase);
        EXPECT_FLOAT_EQ(frame.phase_progress, reference.phase_progress);
        RecordProperty("creation_" + std::to_string(seconds) + "_phase", static_cast<int>(frame.phase));
        RecordProperty("creation_" + std::to_string(seconds) + "_progress", std::to_string(frame.phase_progress));
        auto const thermal = geometry_animator.thermalSample(136U, 118U);
        auto const reference_thermal = control_animator.thermalSample(136U, 118U);
        ASSERT_TRUE(thermal);
        ASSERT_TRUE(reference_thermal);
        EXPECT_FLOAT_EQ(thermal->potential, reference_thermal->potential);
        EXPECT_FLOAT_EQ(thermal->progress, reference_thermal->progress);
        EXPECT_FLOAT_EQ(thermal->elevation_m, reference_thermal->elevation_m);
        if (frame.phase == client::GenerationMapAnimationPhase::Water
            || frame.phase == client::GenerationMapAnimationPhase::Ice)
        {
            intermediate_difference |= frame.surface_image->rgba != reference.surface_image->rgba;
        }
        if (char const* directory = std::getenv("MC_RIVER_MAP_EVIDENCE"))
        {
            std::ofstream output(std::string(directory) + "/creation-" + std::to_string(seconds) + ".ppm",
                std::ios::binary);
            output << "P6\n" << frame.surface_image->width << ' ' << frame.surface_image->height << "\n255\n";
            for (auto const& pixel : frame.surface_image->rgba)
            {
                output.write(reinterpret_cast<char const*>(pixel.data()), 3);
            }
            ASSERT_TRUE(output);
        }
    }
    EXPECT_TRUE(intermediate_difference);
    auto const settled = geometry_animator.updateNative(atlas, source,
        {.visible_elapsed = std::chrono::seconds{90}, .reduced_motion = true});
    ASSERT_TRUE(settled.surface_image);
    EXPECT_EQ(settled.surface_image->rgba, source->rgba);
}

TEST(GenerationMapPresentationTest, MissingMalformedOrNonFiniteHeightProducesNoImage)
{
    static constexpr uint32_t INVALID_RESOLUTION = 128U;

    generation::GenerationSnapshot missing;
    missing.stage = generation::GenerationStage::Tectonics;
    missing.resolution = 256U;
    EXPECT_FALSE(client::rasterizeGenerationMap(missing));

    generation::GenerationSnapshot invalid = makeSnapshot(generation::GenerationStage::Tectonics);
    invalid.resolution = INVALID_RESOLUTION;
    EXPECT_FALSE(client::rasterizeGenerationMap(invalid));

    invalid = makeSnapshot(generation::GenerationStage::Tectonics);
    invalid.elevation.pop_back();
    EXPECT_FALSE(client::rasterizeGenerationMap(invalid));

    invalid = makeSnapshot(generation::GenerationStage::Tectonics);
    invalid.elevation[0U] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(client::rasterizeGenerationMap(invalid));
}

TEST(GenerationMapPresentationTest, UnsupportedCompleteStageProducesNoImage)
{
    generation::GenerationSnapshot snapshot = makeSnapshot(generation::GenerationStage::Complete);

    EXPECT_FALSE(client::rasterizeGenerationMap(snapshot));

    snapshot.stage = static_cast<generation::GenerationStage>(255U);
    EXPECT_FALSE(client::rasterizeGenerationMap(snapshot));
}

TEST(GenerationMapPresentationTest, GeologyAndCoastImagesFollowTheirActualElevationFields)
{
    static constexpr uint32_t LOWLAND_INDEX = 1U;
    static constexpr uint32_t HIGHLAND_INDEX = 2U;

    generation::GenerationSnapshot snapshot = makeSnapshot(generation::GenerationStage::Tectonics);
    snapshot.elevation[0U] = -0.5F;
    snapshot.elevation[LOWLAND_INDEX] = 0.5F;
    snapshot.elevation[HIGHLAND_INDEX] = 3.0F;

    std::optional<client::GenerationMapImage> geology = client::rasterizeGenerationMap(snapshot);
    ASSERT_TRUE(geology);
    ASSERT_EQ(geology->rgba.size(), 256U * 256U);
    EXPECT_NE(geology->rgba[0U], geology->rgba[LOWLAND_INDEX]);
    EXPECT_NE(geology->rgba[LOWLAND_INDEX], geology->rgba[HIGHLAND_INDEX]);

    snapshot.stage = generation::GenerationStage::CoastalElaboration;
    std::optional<client::GenerationMapImage> coast = client::rasterizeGenerationMap(snapshot);
    ASSERT_TRUE(coast);
    EXPECT_EQ(coast->rgba, geology->rgba);

    snapshot.elevation[LOWLAND_INDEX] = 3.0F;
    std::optional<client::GenerationMapImage> elevated_coast = client::rasterizeGenerationMap(snapshot);
    ASSERT_TRUE(elevated_coast);
    EXPECT_NE(elevated_coast->rgba[LOWLAND_INDEX], coast->rgba[LOWLAND_INDEX]);
}

TEST(GenerationMapPresentationTest, WaterImageRequiresAndUsesRealHydrologyFields)
{
    static constexpr uint32_t OCEAN_INDEX = 0U;
    static constexpr uint32_t LAKE_INDEX = 1U;
    static constexpr uint32_t RIVER_INDEX = 2U;
    static constexpr uint32_t LAND_INDEX = 3U;

    generation::GenerationSnapshot snapshot = makeSnapshot(generation::GenerationStage::InitialDrainage);
    EXPECT_FALSE(client::rasterizeGenerationMap(snapshot));

    static constexpr uint32_t FIELD_COUNT = 256U * 256U;
    snapshot.water_level.assign(FIELD_COUNT, 0.5F);
    snapshot.ocean_mask.assign(FIELD_COUNT, 0U);
    snapshot.lake_mask.assign(FIELD_COUNT, 0U);
    snapshot.river_mask.assign(FIELD_COUNT, 0U);
    std::optional<client::GenerationMapImage> unmarked = client::rasterizeGenerationMap(snapshot);
    ASSERT_TRUE(unmarked);

    snapshot.ocean_mask[OCEAN_INDEX] = 1U;
    snapshot.lake_mask[LAKE_INDEX] = 1U;
    snapshot.river_mask[RIVER_INDEX] = 1U;
    snapshot.water_level[OCEAN_INDEX] = 0.75F;
    snapshot.water_level[LAKE_INDEX] = 2.5F;
    std::optional<client::GenerationMapImage> shallow = client::rasterizeGenerationMap(snapshot);
    ASSERT_TRUE(shallow);
    EXPECT_NE(shallow->rgba[OCEAN_INDEX], unmarked->rgba[OCEAN_INDEX]);
    EXPECT_NE(shallow->rgba[LAKE_INDEX], unmarked->rgba[LAKE_INDEX]);
    EXPECT_NE(shallow->rgba[RIVER_INDEX], unmarked->rgba[RIVER_INDEX]);
    EXPECT_EQ(shallow->rgba[LAND_INDEX], unmarked->rgba[LAND_INDEX]);

    snapshot.water_level[OCEAN_INDEX] = 2.0F;
    std::optional<client::GenerationMapImage> deep = client::rasterizeGenerationMap(snapshot);
    ASSERT_TRUE(deep);
    EXPECT_NE(deep->rgba[OCEAN_INDEX], shallow->rgba[OCEAN_INDEX]);
}

TEST(GenerationMapPresentationTest, InvalidWaterFieldsProduceNoImage)
{
    static constexpr uint32_t FIELD_COUNT = 256U * 256U;

    generation::GenerationSnapshot snapshot = makeSnapshot(generation::GenerationStage::FinalDrainage);
    snapshot.water_level.assign(FIELD_COUNT, 0.5F);
    snapshot.ocean_mask.assign(FIELD_COUNT, 0U);
    snapshot.lake_mask.assign(FIELD_COUNT, 0U);
    snapshot.river_mask.assign(FIELD_COUNT, 0U);
    EXPECT_TRUE(client::rasterizeGenerationMap(snapshot));

    snapshot.river_mask[0U] = 2U;
    EXPECT_FALSE(client::rasterizeGenerationMap(snapshot));

    snapshot.river_mask[0U] = 0U;
    snapshot.river_mask.pop_back();
    EXPECT_FALSE(client::rasterizeGenerationMap(snapshot));

    snapshot.river_mask.assign(FIELD_COUNT, 0U);
    snapshot.water_level[0U] = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(client::rasterizeGenerationMap(snapshot));
}

TEST(GenerationMapPresentationTest, FinalDrainageShowsSnowOnColdLandAndIceOnColdOcean)
{
    static constexpr uint32_t FIELD_COUNT = 256U * 256U;
    static constexpr uint32_t COLD_LAND = 0U;
    static constexpr uint32_t WARM_LAND = 1U;
    static constexpr uint32_t WARM_OCEAN = 2U;
    static constexpr uint32_t COLD_OCEAN = 3U;
    static constexpr uint32_t DRY_COLD_LAND = 4U;

    generation::GenerationSnapshot snapshot = makeSnapshot(generation::GenerationStage::FinalDrainage);
    snapshot.elevation[COLD_OCEAN] = -0.5F;
    snapshot.elevation[WARM_OCEAN] = -0.5F;
    addHydrology(snapshot);
    addClimate(snapshot, 5.0F, 0.5F);
    snapshot.ocean_mask[COLD_OCEAN] = 1U;
    snapshot.ocean_mask[WARM_OCEAN] = 1U;
    snapshot.water_level[COLD_OCEAN] = 0.0F;
    snapshot.water_level[WARM_OCEAN] = 0.0F;
    snapshot.temperature[COLD_LAND] = -5.0F;
    snapshot.temperature[COLD_OCEAN] = -5.0F;
    snapshot.temperature[DRY_COLD_LAND] = -5.0F;
    snapshot.precipitation[DRY_COLD_LAND] = 0.0F;
    snapshot.precipitation[COLD_OCEAN] = 0.0F;

    std::vector<uint8_t> display_ice_mask(FIELD_COUNT, client::GENERATION_MAP_ICE_NONE);
    display_ice_mask[COLD_LAND] = client::GENERATION_MAP_ICE_SNOW;
    display_ice_mask[DRY_COLD_LAND] = client::GENERATION_MAP_ICE_SNOW;
    display_ice_mask[COLD_OCEAN] = client::GENERATION_MAP_ICE_WATER;
    std::optional<client::GenerationMapImage> image = client::rasterizeGenerationMap(
        snapshot,
        display_ice_mask
    );

    ASSERT_TRUE(image);
    EXPECT_GT(image->rgba[COLD_LAND][0U], 180U);
    EXPECT_GT(image->rgba[COLD_LAND][1U], 190U);
    EXPECT_GT(image->rgba[COLD_LAND][2U], 190U);
    EXPECT_GT(image->rgba[DRY_COLD_LAND][0U], 180U);
    EXPECT_GT(image->rgba[DRY_COLD_LAND][1U], 190U);
    EXPECT_GT(image->rgba[DRY_COLD_LAND][2U], 190U);
    EXPECT_NE(image->rgba[WARM_LAND], image->rgba[COLD_LAND]);
    EXPECT_LT(image->rgba[COLD_OCEAN][0U], image->rgba[COLD_OCEAN][1U]);
    EXPECT_LT(image->rgba[COLD_OCEAN][1U], image->rgba[COLD_OCEAN][2U]);
    EXPECT_GT(image->rgba[COLD_OCEAN][0U], 150U);
    EXPECT_EQ(image->rgba[WARM_OCEAN][3U], 255U);
    EXPECT_LT(image->rgba[WARM_OCEAN][0U], 150U);
    EXPECT_NE(image->rgba[COLD_OCEAN], image->rgba[WARM_OCEAN]);

    snapshot.stage = generation::GenerationStage::FinalClimate;
    std::optional<client::GenerationMapImage> climate = client::rasterizeGenerationMap(snapshot);
    ASSERT_TRUE(climate);
    EXPECT_NE(image->rgba[COLD_LAND], climate->rgba[COLD_LAND]);
    EXPECT_NE(image->rgba[COLD_OCEAN], climate->rgba[COLD_OCEAN]);
    EXPECT_EQ(FIELD_COUNT, image->rgba.size());
}

TEST(GenerationMapPresentationTest, DisplayIceMaskControlsSnowAndWaterIceComposition)
{
    static constexpr uint32_t FIELD_COUNT = 256U * 256U;
    static constexpr uint32_t JUST_WARM = 0U;
    static constexpr uint32_t AT_BOUNDARY = 1U;
    static constexpr uint32_t JUST_COLD = 2U;
    static constexpr uint32_t OCEAN_JUST_WARM = 3U;
    static constexpr uint32_t OCEAN_AT_BOUNDARY = 4U;
    static constexpr uint32_t OCEAN_JUST_COLD = 5U;

    generation::GenerationSnapshot snapshot = makeSnapshot(generation::GenerationStage::FinalDrainage);
    addHydrology(snapshot);
    addClimate(snapshot, 5.0F, 0.5F);
    snapshot.ocean_mask[OCEAN_JUST_WARM] = 1U;
    snapshot.ocean_mask[OCEAN_AT_BOUNDARY] = 1U;
    snapshot.ocean_mask[OCEAN_JUST_COLD] = 1U;
    snapshot.elevation[OCEAN_JUST_WARM] = -0.5F;
    snapshot.elevation[OCEAN_AT_BOUNDARY] = -0.5F;
    snapshot.elevation[OCEAN_JUST_COLD] = -0.5F;

    std::vector<uint8_t> display_ice_mask(FIELD_COUNT, client::GENERATION_MAP_ICE_NONE);
    display_ice_mask[AT_BOUNDARY] = client::GENERATION_MAP_ICE_SNOW;
    display_ice_mask[JUST_COLD] = client::GENERATION_MAP_ICE_SNOW;
    display_ice_mask[OCEAN_JUST_COLD] = client::GENERATION_MAP_ICE_WATER;
    std::optional<client::GenerationMapImage> image = client::rasterizeGenerationMap(
        snapshot,
        display_ice_mask
    );
    ASSERT_TRUE(image);

    EXPECT_GT(maximumChannelDistance(image->rgba[AT_BOUNDARY], image->rgba[JUST_WARM]), 40U);
    EXPECT_GT(image->rgba[AT_BOUNDARY][0U], image->rgba[JUST_WARM][0U]);
    EXPECT_GT(image->rgba[AT_BOUNDARY][1U], image->rgba[JUST_WARM][1U]);
    EXPECT_GT(image->rgba[AT_BOUNDARY][2U], image->rgba[JUST_WARM][2U]);
    EXPECT_GT(image->rgba[JUST_COLD][0U], 180U);
    EXPECT_GT(image->rgba[JUST_COLD][1U], 190U);
    EXPECT_GT(image->rgba[JUST_COLD][2U], 190U);
    EXPECT_GT(image->rgba[AT_BOUNDARY][0U], 180U);
    EXPECT_GT(image->rgba[AT_BOUNDARY][1U], 190U);
    EXPECT_GT(image->rgba[AT_BOUNDARY][2U], 190U);
    EXPECT_GT(
        maximumChannelDistance(image->rgba[OCEAN_JUST_COLD], image->rgba[OCEAN_AT_BOUNDARY]),
        40U
    );
    EXPECT_LT(image->rgba[OCEAN_AT_BOUNDARY][0U], 120U);
    EXPECT_LT(image->rgba[OCEAN_AT_BOUNDARY][2U], 160U);
    EXPECT_GT(image->rgba[OCEAN_JUST_COLD][0U], image->rgba[OCEAN_AT_BOUNDARY][0U]);
    EXPECT_GT(image->rgba[OCEAN_JUST_COLD][1U], image->rgba[OCEAN_AT_BOUNDARY][1U]);
    EXPECT_GT(image->rgba[OCEAN_JUST_COLD][2U], image->rgba[OCEAN_AT_BOUNDARY][2U]);
}

TEST(GenerationMapPresentationTest, SharedDisplayIceMaskUsesSnowZonesAndColdWaterThreshold)
{
    std::array<uint8_t, 5U> climate_zone{ 5U, 5U, 0U, 0U, 0U };
    std::array<uint8_t, 5U> ocean_mask{ 0U, 1U, 1U, 0U, 0U };
    std::array<uint8_t, 5U> lake_mask{ 0U, 0U, 0U, 1U, 0U };
    std::array<float, 5U> temperature{ -3.0F, -20.0F, -10.0F, -10.0F, -10.0F };

    std::optional<client::GenerationMapDisplayIceMask> const mask = client::makeGenerationMapDisplayIceMask(
        climate_zone,
        ocean_mask,
        lake_mask,
        temperature
    );

    ASSERT_TRUE(mask);
    EXPECT_EQ(mask->surface_kind, (std::vector<uint8_t>{
        client::GENERATION_MAP_ICE_SNOW,
        client::GENERATION_MAP_ICE_WATER,
        client::GENERATION_MAP_ICE_WATER,
        client::GENERATION_MAP_ICE_WATER,
        client::GENERATION_MAP_ICE_NONE,
    }));
    EXPECT_TRUE(std::equal(mask->temperature.begin(), mask->temperature.end(), temperature.begin()));
}

TEST(GenerationMapPresentationTest, GlobalAtlasRasterMatchesSnapshotForSharedFinalMask)
{
    static constexpr uint32_t FIELD_COUNT = 256U * 256U;
    static constexpr uint32_t SNOW_INDEX = 0U;
    static constexpr uint32_t WATER_ICE_INDEX = 1U;

    generation::GenerationSnapshot snapshot = makeSnapshot(generation::GenerationStage::FinalDrainage);
    addHydrology(snapshot);
    addClimate(snapshot, -20.0F, 0.5F);
    snapshot.ocean_mask[WATER_ICE_INDEX] = 1U;

    generation::GlobalAtlas atlas;
    atlas.configuration.resolution = snapshot.resolution;
    atlas.elevation = snapshot.elevation;
    atlas.water_level = snapshot.water_level;
    atlas.ocean_mask = snapshot.ocean_mask;
    atlas.lake_mask = snapshot.lake_mask;
    atlas.river_mask = snapshot.river_mask;
    atlas.temperature = snapshot.temperature;
    atlas.precipitation = snapshot.precipitation;
    atlas.climate_zone.assign(FIELD_COUNT, 0U);
    atlas.climate_zone[SNOW_INDEX] = 5U;

    std::optional<client::GenerationMapDisplayIceMask> const display_ice_mask =
        client::makeGenerationMapDisplayIceMask(
            atlas.climate_zone,
            atlas.ocean_mask,
            atlas.lake_mask,
            atlas.temperature
        );
    ASSERT_TRUE(display_ice_mask);

    std::optional<client::GenerationMapImage> const snapshot_image = client::rasterizeGenerationMap(
        snapshot,
        display_ice_mask->surface_kind
    );
    std::optional<client::GenerationMapImage> const atlas_image = client::rasterizeGenerationMap(
        atlas,
        display_ice_mask->surface_kind
    );

    ASSERT_TRUE(snapshot_image);
    ASSERT_TRUE(atlas_image);
    EXPECT_EQ(atlas_image->rgba, snapshot_image->rgba);
}

TEST(GenerationMapPresentationTest, AnimationFillsOnlyCellsInActualHydrologyMasks)
{
    static constexpr uint32_t OCEAN_INDEX = 0U;
    generation::GenerationSnapshot snapshot = makeSnapshot(generation::GenerationStage::InitialDrainage);
    addHydrology(snapshot);
    snapshot.elevation[OCEAN_INDEX] = -0.3F;
    snapshot.ocean_mask[OCEAN_INDEX] = 1U;
    snapshot.water_level[OCEAN_INDEX] = 1.0F;
    generation::GenerationSnapshot without_water = snapshot;
    without_water.ocean_mask[OCEAN_INDEX] = 0U;

    client::GenerationMapAnimationClock const clock{
        .visible_elapsed = std::chrono::milliseconds{ 12'000 },
    };
    std::shared_ptr<client::GenerationMapImage const> const water = client::animateGenerationMap(
        snapshot,
        nullptr,
        nullptr,
        clock
    );
    std::shared_ptr<client::GenerationMapImage const> const land = client::animateGenerationMap(
        without_water,
        nullptr,
        nullptr,
        clock
    );

    ASSERT_NE(water, nullptr);
    ASSERT_NE(land, nullptr);
    EXPECT_NE(
        water->rgba[animationRenderPixelIndex(0U, 0U)],
        land->rgba[animationRenderPixelIndex(0U, 0U)]
    );
    EXPECT_EQ(
        water->rgba[animationRenderPixelIndex(1U, 0U)],
        land->rgba[animationRenderPixelIndex(1U, 0U)]
    );
}

TEST(GenerationMapPresentationTest, AnimationUsesTheSharedMaskAndDoesNotDelayReadiness)
{
    static constexpr uint32_t COLDER_INDEX = 0U;
    static constexpr uint32_t WARMER_SOURCE_INDEX = 2U;
    static constexpr uint32_t FIELD_COUNT = 256U * 256U;

    generation::GenerationSnapshot snapshot = makeSnapshot(generation::GenerationStage::FinalDrainage);
    addHydrology(snapshot);
    addClimate(snapshot, 0.0F, 0.5F);
    std::vector<uint8_t> climate_zone(FIELD_COUNT, 0U);
    std::vector<uint8_t> ocean_mask(FIELD_COUNT, 0U);
    std::vector<uint8_t> lake_mask(FIELD_COUNT, 0U);
    std::vector<float> target_temperature(FIELD_COUNT, 0.0F);
    climate_zone[COLDER_INDEX] = 5U;
    climate_zone[WARMER_SOURCE_INDEX] = 5U;
    target_temperature[COLDER_INDEX] = -20.0F;
    target_temperature[WARMER_SOURCE_INDEX] = -2.0F;
    snapshot.temperature[COLDER_INDEX] = -20.0F;
    snapshot.temperature[WARMER_SOURCE_INDEX] = -2.0F;
    std::optional<client::GenerationMapDisplayIceMask> const mask = client::makeGenerationMapDisplayIceMask(
        climate_zone,
        ocean_mask,
        lake_mask,
        target_temperature
    );
    ASSERT_TRUE(mask);
    auto const mask_handle = std::make_shared<client::GenerationMapDisplayIceMask const>(*mask);

    auto final_pixels = client::rasterizeGenerationMap(snapshot, mask_handle->surface_kind);
    ASSERT_TRUE(final_pixels);
    auto const final_image = std::make_shared<client::GenerationMapImage const>(std::move(*final_pixels));
    auto const early = client::animateGenerationMap(
        snapshot,
        nullptr,
        mask_handle,
        {.visible_elapsed = std::chrono::milliseconds{25'000}}
    );
    auto const unmasked_early = client::animateGenerationMap(
        snapshot,
        nullptr,
        nullptr,
        {.visible_elapsed = std::chrono::milliseconds{25'000}}
    );
    ASSERT_NE(early, nullptr);
    ASSERT_NE(unmasked_early, nullptr);
    EXPECT_EQ(early->width, client::GENERATION_MAP_ANIMATION_RENDER_RESOLUTION);
    EXPECT_EQ(final_image->width, snapshot.resolution);
    uint32_t const colder_pixel = animationRenderPixelIndex(COLDER_INDEX, 0U);
    uint32_t const warmer_pixel = animationRenderPixelIndex(WARMER_SOURCE_INDEX, 0U);
    EXPECT_NE(early->rgba[colder_pixel], unmasked_early->rgba[colder_pixel]);
    EXPECT_EQ(early->rgba[warmer_pixel], unmasked_early->rgba[warmer_pixel]);

    auto const settled = client::animateGenerationMap(
        snapshot,
        final_image,
        mask_handle,
        {.visible_elapsed = std::chrono::milliseconds{37'500}}
    );
    EXPECT_EQ(settled, final_image);
    auto const ready = client::animateGenerationMap(
        snapshot,
        final_image,
        mask_handle,
        {.visible_elapsed = std::chrono::milliseconds{25'000}, .ready = true}
    );
    EXPECT_EQ(ready, final_image);
}

TEST(GenerationMapPresentationTest, AnimationIsBoundedDeterministicAndSettlesToTheSharedFinalImage)
{
    generation::GenerationSnapshot snapshot = makeSnapshot(generation::GenerationStage::Tectonics);
    for (uint32_t y = 0U; y < snapshot.resolution; ++y)
    {
        for (uint32_t x = 0U; x < snapshot.resolution; ++x)
        {
            snapshot.elevation[y * snapshot.resolution + x] = static_cast<float>(x) / 64.0F;
        }
    }

    client::GenerationMapAnimationClock const early_clock{
        .visible_elapsed = std::chrono::milliseconds{ 0 },
    };
    client::GenerationMapAnimationClock const late_clock{
        .visible_elapsed = std::chrono::milliseconds{ 1'750 },
    };
    std::shared_ptr<client::GenerationMapImage const> const early = client::animateGenerationMap(
        snapshot,
        nullptr,
        nullptr,
        early_clock
    );
    std::shared_ptr<client::GenerationMapImage const> const late = client::animateGenerationMap(
        snapshot,
        nullptr,
        nullptr,
        late_clock
    );

    ASSERT_NE(early, nullptr);
    ASSERT_NE(late, nullptr);
    EXPECT_EQ(early->width, client::GENERATION_MAP_ANIMATION_RENDER_RESOLUTION);
    EXPECT_EQ(early->height, client::GENERATION_MAP_ANIMATION_RENDER_RESOLUTION);
    EXPECT_NE(early->rgba, late->rgba);

    std::optional<client::GenerationMapImage> final_image_value = client::rasterizeGenerationMap(snapshot);
    ASSERT_TRUE(final_image_value);
    client::GenerationMapImageHandle const final_image = std::make_shared<client::GenerationMapImage const>(
        std::move(*final_image_value)
    );
    client::GenerationMapAnimationClock const ready_clock{
        .visible_elapsed = std::chrono::milliseconds{ 0 },
        .ready = true,
    };

    EXPECT_EQ(client::animateGenerationMap(snapshot, final_image, nullptr, ready_clock), final_image);
}

TEST(GenerationMapPresentationTest, TectonicDriftKeepsNearestSharpnessAndStaysContinuousAcrossRelief)
{
    static constexpr uint32_t NATIVE_EDGE_X = 128U;
    static constexpr uint32_t EDGE_WINDOW_RADIUS = 8U;
    static constexpr uint32_t EDGE_ROW = 100U * client::GENERATION_MAP_ANIMATION_SCALE;
    static constexpr uint32_t HIGH_COLOR_THRESHOLD = 130U;

    std::shared_ptr<generation::GlobalAtlas const> const atlas = makeDriftingMapAtlas();
    client::GenerationMapAnimator animator;
    std::array<uint64_t, 6U> elapsed_microseconds{};
    std::array<uint8_t, 6U> phase_indices{};
    uint32_t frame_count = 0U;
    auto renderAt = [&](std::chrono::milliseconds const visible_elapsed) {
        auto const start = std::chrono::steady_clock::now();
        client::GenerationMapAnimationFrame frame = animator.update(
            nullptr,
            atlas,
            nullptr,
            nullptr,
            client::GenerationMapAnimationClock{
                .visible_elapsed = visible_elapsed,
                .formation_duration = std::chrono::seconds{10},
            }
        );
        elapsed_microseconds[frame_count] = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start
            ).count()
        );
        phase_indices[frame_count] = static_cast<uint8_t>(frame.phase);
        ++frame_count;
        return frame;
    };

    client::GenerationMapAnimationFrame const at_start = renderAt(std::chrono::milliseconds{ 0 });
    client::GenerationMapAnimationFrame const at_fractional_drift = renderAt(std::chrono::milliseconds{ 7'500 });
    client::GenerationMapAnimationFrame const after_fractional_drift = renderAt(std::chrono::milliseconds{ 7'750 });
    client::GenerationMapAnimationFrame const relief_after_drift = renderAt(std::chrono::milliseconds{ 8'000 });
    client::GenerationMapAnimationFrame const before_relief = renderAt(std::chrono::milliseconds{ 9'749 });
    client::GenerationMapAnimationFrame const relief_start = renderAt(std::chrono::milliseconds{ 9'750 });

    ASSERT_NE(at_start.image, nullptr);
    ASSERT_NE(at_fractional_drift.image, nullptr);
    ASSERT_NE(after_fractional_drift.image, nullptr);
    ASSERT_NE(before_relief.image, nullptr);
    ASSERT_NE(relief_start.image, nullptr);
    ASSERT_NE(relief_after_drift.image, nullptr);
    EXPECT_EQ(at_start.image->width, client::GENERATION_MAP_ANIMATION_RENDER_RESOLUTION);
    EXPECT_EQ(at_start.image->height, client::GENERATION_MAP_ANIMATION_RENDER_RESOLUTION);
    EXPECT_EQ(at_start.phase, client::GenerationMapAnimationPhase::Tectonics);
    EXPECT_EQ(at_fractional_drift.phase, client::GenerationMapAnimationPhase::Relief);
    EXPECT_EQ(relief_start.phase, client::GenerationMapAnimationPhase::Ice);

    uint32_t const center_edge = NATIVE_EDGE_X * client::GENERATION_MAP_ANIMATION_SCALE
        - 6U * client::GENERATION_MAP_ANIMATION_SCALE;
    uint32_t const edge_first_x = center_edge - EDGE_WINDOW_RADIUS;
    uint32_t const edge_last_x = center_edge + EDGE_WINDOW_RADIUS;
    double const start_edge = verticalEdgePosition(
        *at_fractional_drift.image, EDGE_ROW, edge_first_x, edge_last_x
    );
    double const fractional_edge = verticalEdgePosition(
        *after_fractional_drift.image,
        EDGE_ROW,
        edge_first_x,
        edge_last_x
    );
    double const fractional_shift = fractional_edge - start_edge;
    EXPECT_GE(fractional_shift, 0.0);
    EXPECT_LE(fractional_shift, 1.0);
    EXPECT_GT(
        verticalEdgePosition(*relief_after_drift.image, EDGE_ROW, edge_first_x, edge_last_x) - start_edge,
        0.5
    );

    double const phase_boundary_shift = verticalEdgePosition(
        *relief_start.image,
        EDGE_ROW,
        edge_first_x,
        edge_last_x
    ) - verticalEdgePosition(*before_relief.image, EDGE_ROW, edge_first_x, edge_last_x);
    EXPECT_NEAR(phase_boundary_shift, 0.0, 0.1);

    uint32_t const seam_row = EDGE_ROW * at_start.image->width;
    EXPECT_GT(at_start.image->rgba[seam_row][2U], HIGH_COLOR_THRESHOLD);
    EXPECT_GT(at_start.image->rgba[seam_row + at_start.image->width - 1U][2U], HIGH_COLOR_THRESHOLD);
    EXPECT_TRUE(std::all_of(at_start.image->rgba.begin(), at_start.image->rgba.end(), [](Rgba8 const& pixel) {
        return pixel[3U] == 255U;
    }));

    EXPECT_TRUE(writePpmIfRequested("tectonics-0000.ppm", *at_start.image));
    EXPECT_TRUE(writePpmIfRequested("relief-7500.ppm", *at_fractional_drift.image));
    EXPECT_TRUE(writePpmIfRequested("relief-7750.ppm", *after_fractional_drift.image));
    EXPECT_TRUE(writePpmIfRequested("relief-9749.ppm", *before_relief.image));
    EXPECT_TRUE(writePpmIfRequested("ice-9750.ppm", *relief_start.image));
    EXPECT_TRUE(writePpmIfRequested("relief-8000.ppm", *relief_after_drift.image));
    EXPECT_TRUE(writeFrameTimingsIfRequested(elapsed_microseconds, phase_indices));
}

TEST(GenerationMapPresentationTest, TectonicDriftPreservesWaterInteriorColorAndShadingSemantics)
{
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr uint32_t FIELD_COUNT = RESOLUTION * RESOLUTION;
    // Flooded cell beside the translated relief: only the dry control receives hillshade.
    static constexpr uint32_t SAMPLE_X = 140U;
    static constexpr uint32_t SAMPLE_Y = 100U;

    auto water_atlas = std::make_shared<generation::GlobalAtlas>(*makeDriftingMapAtlas());
    water_atlas->sea_level = 20.0F;
    water_atlas->water_level.assign(FIELD_COUNT, 20.0F);
    water_atlas->ocean_mask.assign(FIELD_COUNT, 1U);
    water_atlas->lake_mask.assign(FIELD_COUNT, 0U);
    water_atlas->river_mask.assign(FIELD_COUNT, 0U);
    water_atlas->temperature.assign(FIELD_COUNT, 15.0F);
    water_atlas->precipitation.assign(FIELD_COUNT, 0.0F);
    for (uint32_t y = 0U; y < RESOLUTION; ++y)
    {
        for (uint32_t x = 0U; x < RESOLUTION; ++x)
        {
            uint32_t const index = y * RESOLUTION + x;
            bool const right_plate = x >= RESOLUTION / 2U;
            water_atlas->elevation[index] = right_plate ? 18.0F : -3.5F;
            water_atlas->plate_id[index] = right_plate ? 1 : 0;
            water_atlas->plate_vx[index] = right_plate ? 1.0F : -1.0F;
            water_atlas->plate_vy[index] = 0.0F;
        }
    }
    auto dry_control_atlas = std::make_shared<generation::GlobalAtlas>(*water_atlas);
    std::fill(dry_control_atlas->ocean_mask.begin(), dry_control_atlas->ocean_mask.end(), 0U);
    auto const final_source_image = client::rasterizeGenerationMap(*water_atlas, {}, false);
    ASSERT_TRUE(final_source_image);

    client::GenerationMapAnimator water_animator;
    client::GenerationMapAnimator dry_control_animator;
    client::GenerationMapAnimationClock const clock{
        .visible_elapsed = std::chrono::milliseconds{ 21'500 },
    };
    client::GenerationMapAnimationFrame const water_frame = water_animator.update(
        nullptr,
        std::move(water_atlas),
        nullptr,
        nullptr,
        clock
    );
    client::GenerationMapAnimationFrame const dry_control_frame = dry_control_animator.update(
        nullptr,
        std::move(dry_control_atlas),
        nullptr,
        nullptr,
        clock
    );

    ASSERT_NE(water_frame.image, nullptr);
    ASSERT_NE(dry_control_frame.image, nullptr);
    EXPECT_TRUE(writePpmIfRequested("tectonics-water.ppm", *water_frame.image));
    EXPECT_TRUE(writePpmIfRequested("tectonics-dry-control.ppm", *dry_control_frame.image));
    uint32_t const sample_index = animationRenderPixelIndex(SAMPLE_X, SAMPLE_Y);
    Rgba8 const water_pixel = water_frame.image->rgba[sample_index];
    Rgba8 const dry_control_pixel = dry_control_frame.image->rgba[sample_index];
    uint32_t const final_sample_index = SAMPLE_Y * final_source_image->width + SAMPLE_X;
    EXPECT_EQ(water_pixel, final_source_image->rgba[final_sample_index]);
    EXPECT_NE(water_pixel, dry_control_pixel);
    EXPECT_EQ(water_pixel[3U], 255U);
}

TEST(GenerationMapPresentationTest, TectonicDriftUsesMappedHeightScaleForReliefShading)
{
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr uint32_t RAMP_START_X = 64U;
    static constexpr uint32_t RAMP_END_X = 80U;
    static constexpr uint32_t MOVING_PLATE_END = 128U;
    static constexpr uint32_t SAMPLE_X = 33U;
    static constexpr uint32_t SAMPLE_Y = 50U;
    static constexpr Rgba8 EXPECTED_UNSHADED_COLOR{ 229U, 231U, 224U, 255U };

    auto atlas = std::make_shared<generation::GlobalAtlas>(*makeDriftingMapAtlas());
    for (uint32_t y = 0U; y < RESOLUTION; ++y)
    {
        for (uint32_t x = 0U; x < RESOLUTION; ++x)
        {
            uint32_t const index = y * RESOLUTION + x;
            bool const moving_plate = x < MOVING_PLATE_END;
            atlas->elevation[index] = x < RAMP_START_X
                ? 8.0F
                : (x < RAMP_END_X ? 8.0F + static_cast<float>(x - RAMP_START_X) * 0.625F : 18.0F);
            atlas->plate_id[index] = moving_plate ? 0 : 1;
            atlas->plate_vx[index] = moving_plate ? 1.0F : 0.0F;
            atlas->plate_vy[index] = 0.0F;
        }
    }

    client::GenerationMapAnimator animator;
    client::GenerationMapAnimationFrame const frame = animator.update(
        nullptr,
        std::move(atlas),
        nullptr,
        nullptr,
        client::GenerationMapAnimationClock{ .visible_elapsed = std::chrono::milliseconds{ 0 } }
    );

    ASSERT_NE(frame.image, nullptr);
    uint32_t const sample_index = animationRenderPixelIndex(SAMPLE_X, SAMPLE_Y);
    EXPECT_EQ(frame.image->rgba[sample_index], EXPECTED_UNSHADED_COLOR);
}

TEST(GenerationMapPresentationTest, HigherMovingPlateKeepsOwnershipWhenItOverlapsStationaryTerrain)
{
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr uint32_t MOVING_PLATE_START = 128U;
    static constexpr uint32_t MOVING_PLATE_END = 160U;
    static constexpr uint32_t OVERLAP_NATIVE_X = 160U;
    static constexpr uint32_t SAMPLE_Y = 100U;
    static constexpr uint32_t HIGH_COLOR_THRESHOLD = 130U;

    auto atlas = std::make_shared<generation::GlobalAtlas>(*makeDriftingMapAtlas());
    std::fill(atlas->plate_id.begin(), atlas->plate_id.end(), 0);
    std::fill(atlas->plate_vx.begin(), atlas->plate_vx.end(), 0.0F);
    for (uint32_t y = 0U; y < RESOLUTION; ++y)
    {
        for (uint32_t x = 0U; x < RESOLUTION; ++x)
        {
            uint32_t const index = y * RESOLUTION + x;
            bool const is_moving_plate = x >= MOVING_PLATE_START && x < MOVING_PLATE_END;
            atlas->elevation[index] = is_moving_plate ? 100.0F : -100.0F;
            atlas->plate_id[index] = is_moving_plate ? 1 : 0;
            atlas->plate_vx[index] = is_moving_plate ? -1.0F : 0.0F;
        }
    }

    client::GenerationMapAnimator animator;
    client::GenerationMapAnimationFrame const frame = animator.update(
        nullptr,
        std::move(atlas),
        nullptr,
        nullptr,
        client::GenerationMapAnimationClock{ .visible_elapsed = std::chrono::milliseconds{ 21'500 } }
    );

    ASSERT_NE(frame.image, nullptr);
    uint32_t const overlap_pixel = SAMPLE_Y * client::GENERATION_MAP_ANIMATION_SCALE
            * frame.image->width + OVERLAP_NATIVE_X * client::GENERATION_MAP_ANIMATION_SCALE + 1U;
    EXPECT_GT(frame.image->rgba[overlap_pixel][2U], HIGH_COLOR_THRESHOLD);
}

TEST(GenerationMapPresentationTest, MovingDriftUsesNearestWaterColorToFillNewGaps)
{
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr uint32_t WATER_X = 124U;
    static constexpr uint32_t WATER_Y = 200U;
    static constexpr uint32_t GAP_PIXEL_X = 253U;
    static constexpr uint32_t GAP_PIXEL_Y = 400U;
    static constexpr uint32_t HIGH_COLOR_THRESHOLD = 120U;

    auto atlas = std::make_shared<generation::GlobalAtlas>(*makeDriftingMapAtlas());
    uint32_t const field_count = RESOLUTION * RESOLUTION;
    atlas->water_level.assign(field_count, 0.0F);
    atlas->ocean_mask.assign(field_count, 0U);
    atlas->lake_mask.assign(field_count, 0U);
    atlas->river_mask.assign(field_count, 0U);
    uint32_t const water_index = WATER_Y * RESOLUTION + WATER_X;
    atlas->elevation[water_index] = 100.0F;
    atlas->ocean_mask[water_index] = 1U;
    for (uint32_t y = 0U; y < RESOLUTION; ++y)
    {
        for (uint32_t x = 0U; x < RESOLUTION; ++x)
        {
            uint32_t const index = y * RESOLUTION + x;
            bool const east_plate = x >= RESOLUTION / 2U;
            atlas->plate_id[index] = east_plate ? 1 : 0;
            atlas->plate_vx[index] = east_plate ? -1.0F : 1.0F;
        }
    }

    client::GenerationMapAnimator animator;
    client::GenerationMapAnimationFrame const frame = animator.update(
        nullptr,
        std::move(atlas),
        nullptr,
        nullptr,
        client::GenerationMapAnimationClock{ .visible_elapsed = std::chrono::milliseconds{ 21'500 } }
    );

    ASSERT_NE(frame.image, nullptr);
    uint32_t const gap_pixel = GAP_PIXEL_Y * frame.image->width + GAP_PIXEL_X;
    EXPECT_GT(frame.image->rgba[gap_pixel][2U], HIGH_COLOR_THRESHOLD);
}

TEST(GenerationMapPresentationTest, ReducedMotionUsesAStableSnapshotEndpoint)
{
    generation::GenerationSnapshot snapshot = makeSnapshot(generation::GenerationStage::Tectonics);
    std::optional<client::GenerationMapImage> const snapshot_image = client::rasterizeGenerationMap(snapshot);
    ASSERT_TRUE(snapshot_image);
    client::GenerationMapAnimationClock const early_clock{
        .visible_elapsed = std::chrono::milliseconds{ 0 },
        .reduced_motion = true,
    };
    client::GenerationMapAnimationClock const late_clock{
        .visible_elapsed = std::chrono::milliseconds{ 3'600'000 },
        .reduced_motion = true,
    };

    std::shared_ptr<client::GenerationMapImage const> const early = client::animateGenerationMap(
        snapshot,
        nullptr,
        nullptr,
        early_clock
    );
    std::shared_ptr<client::GenerationMapImage const> const late = client::animateGenerationMap(
        snapshot,
        nullptr,
        nullptr,
        late_clock
    );

    ASSERT_NE(early, nullptr);
    ASSERT_NE(late, nullptr);
    EXPECT_EQ(early->rgba, snapshot_image->rgba);
    EXPECT_EQ(late->rgba, snapshot_image->rgba);

    client::GenerationMapAnimator animator;
    auto const snapshot_handle = std::make_shared<generation::GenerationSnapshot const>(snapshot);
    client::GenerationMapAnimationFrame const static_frame = animator.update(
        snapshot_handle,
        nullptr,
        nullptr,
        nullptr,
        late_clock
    );
    ASSERT_NE(static_frame.image, nullptr);
    EXPECT_EQ(static_frame.image->rgba, snapshot_image->rgba);
}

TEST(GenerationMapPresentationTest, ReducedMotionKeepsTheLatestHydrologySnapshot)
{
    static constexpr uint32_t OCEAN_INDEX = 0U;
    generation::GenerationSnapshot snapshot = makeSnapshot(generation::GenerationStage::FinalDrainage);
    addHydrology(snapshot);
    snapshot.elevation[OCEAN_INDEX] = -0.3F;
    snapshot.ocean_mask[OCEAN_INDEX] = 1U;
    snapshot.water_level[OCEAN_INDEX] = 1.0F;
    generation::GenerationSnapshot without_water = snapshot;
    without_water.ocean_mask[OCEAN_INDEX] = 0U;

    std::optional<client::GenerationMapImage> const expected = client::rasterizeGenerationMap(snapshot);
    std::optional<client::GenerationMapImage> const dry = client::rasterizeGenerationMap(without_water);
    ASSERT_TRUE(expected);
    ASSERT_TRUE(dry);
    EXPECT_NE(expected->rgba[OCEAN_INDEX], dry->rgba[OCEAN_INDEX]);

    client::GenerationMapAnimationClock const clock{
        .visible_elapsed = std::chrono::milliseconds{ 3'600'000 },
        .reduced_motion = true,
    };
    std::shared_ptr<client::GenerationMapImage const> const image = client::animateGenerationMap(
        snapshot,
        nullptr,
        nullptr,
        clock
    );
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(image->rgba, expected->rgba);
}

TEST(GenerationMapPresentationTest, MissingClimateDoesNotBreakFinalHydrologyOrPreClimateStages)
{
    static constexpr uint32_t FIELD_COUNT = 256U * 256U;
    static constexpr uint32_t COLD_LAND = 0U;
    static constexpr uint32_t OCEAN = 1U;

    generation::GenerationSnapshot snapshot = makeSnapshot(generation::GenerationStage::FinalDrainage);
    snapshot.elevation[OCEAN] = -0.5F;
    addHydrology(snapshot);
    snapshot.ocean_mask[OCEAN] = 1U;
    snapshot.water_level[OCEAN] = 0.0F;

    std::optional<client::GenerationMapImage> without_climate = client::rasterizeGenerationMap(snapshot);
    ASSERT_TRUE(without_climate);

    addClimate(snapshot, -5.0F, 0.5F);
    std::optional<client::GenerationMapImage> with_climate = client::rasterizeGenerationMap(snapshot);
    ASSERT_TRUE(with_climate);
    EXPECT_NE(with_climate->rgba[COLD_LAND], without_climate->rgba[COLD_LAND]);
    EXPECT_NE(with_climate->rgba[OCEAN], without_climate->rgba[OCEAN]);

    snapshot.stage = generation::GenerationStage::InitialDrainage;
    std::optional<client::GenerationMapImage> pre_climate = client::rasterizeGenerationMap(snapshot);
    ASSERT_TRUE(pre_climate);
    EXPECT_EQ(pre_climate->rgba, without_climate->rgba);

    snapshot.stage = generation::GenerationStage::FinalDrainage;
    snapshot.precipitation.clear();
    std::optional<client::GenerationMapImage> missing_precipitation = client::rasterizeGenerationMap(snapshot);
    ASSERT_TRUE(missing_precipitation);
    EXPECT_EQ(missing_precipitation->rgba, without_climate->rgba);
    EXPECT_EQ(FIELD_COUNT, missing_precipitation->rgba.size());
}

TEST(GenerationMapPresentationTest, ClimateImageUsesActualTemperatureAndPrecipitation)
{
    static constexpr uint32_t SNOW_INDEX = 0U;
    static constexpr uint32_t ARID_INDEX = 1U;
    static constexpr uint32_t HUMID_INDEX = 2U;
    static constexpr uint32_t FIELD_COUNT = 256U * 256U;

    generation::GenerationSnapshot snapshot = makeSnapshot(generation::GenerationStage::FinalClimate);
    snapshot.temperature.assign(FIELD_COUNT, 5.0F);
    snapshot.precipitation.assign(FIELD_COUNT, 0.5F);
    snapshot.temperature[SNOW_INDEX] = -5.0F;
    snapshot.precipitation[SNOW_INDEX] = 0.8F;
    snapshot.temperature[ARID_INDEX] = 25.0F;
    snapshot.precipitation[ARID_INDEX] = 0.1F;
    snapshot.temperature[HUMID_INDEX] = 25.0F;
    snapshot.precipitation[HUMID_INDEX] = 2.0F;

    std::optional<client::GenerationMapImage> climate = client::rasterizeGenerationMap(snapshot);
    ASSERT_TRUE(climate);
    EXPECT_NE(climate->rgba[SNOW_INDEX], climate->rgba[ARID_INDEX]);
    EXPECT_NE(climate->rgba[ARID_INDEX], climate->rgba[HUMID_INDEX]);

    std::optional<client::GenerationMapImage> repeated = client::rasterizeGenerationMap(snapshot);
    ASSERT_TRUE(repeated);
    EXPECT_EQ(repeated->rgba, climate->rgba);

    snapshot.precipitation[HUMID_INDEX] = 0.1F;
    std::optional<client::GenerationMapImage> drier = client::rasterizeGenerationMap(snapshot);
    ASSERT_TRUE(drier);
    EXPECT_NE(drier->rgba[HUMID_INDEX], climate->rgba[HUMID_INDEX]);
}

TEST(GenerationMapPresentationTest, IncompleteClimateDataProducesNoImage)
{
    static constexpr uint32_t FIELD_COUNT = 256U * 256U;

    generation::GenerationSnapshot snapshot = makeSnapshot(generation::GenerationStage::ProvisionalClimate);
    snapshot.temperature.assign(FIELD_COUNT, 5.0F);
    EXPECT_FALSE(client::rasterizeGenerationMap(snapshot));

    snapshot.precipitation.assign(FIELD_COUNT - 1U, 0.5F);
    EXPECT_FALSE(client::rasterizeGenerationMap(snapshot));

    snapshot.precipitation.assign(FIELD_COUNT, 0.5F);
    snapshot.temperature[0U] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(client::rasterizeGenerationMap(snapshot));

    snapshot = makeSnapshot(generation::GenerationStage::ProvisionalClimate);
    snapshot.temperature.assign(FIELD_COUNT, 5.0F);
    snapshot.precipitation.assign(FIELD_COUNT, 0.5F);
    snapshot.elevation.clear();
    EXPECT_FALSE(client::rasterizeGenerationMap(snapshot));
}

TEST(GenerationMapPresentationTest, ClimateTintPreservesElevationDifferences)
{
    static constexpr uint32_t FIELD_COUNT = 256U * 256U;
    static constexpr uint32_t LOWER_INDEX = 0U;
    static constexpr uint32_t HIGHER_INDEX = 1U;

    generation::GenerationSnapshot snapshot = makeSnapshot(generation::GenerationStage::FinalClimate);
    snapshot.elevation[LOWER_INDEX] = 0.4F;
    snapshot.elevation[HIGHER_INDEX] = 2.7F;
    snapshot.temperature.assign(FIELD_COUNT, 8.0F);
    snapshot.precipitation.assign(FIELD_COUNT, 0.6F);

    std::optional<client::GenerationMapImage> image = client::rasterizeGenerationMap(snapshot);

    ASSERT_TRUE(image);
    EXPECT_NE(image->rgba[LOWER_INDEX], image->rgba[HIGHER_INDEX]);
}

TEST(GenerationMapPresentationTest, RiverTintRetainsTerrainContext)
{
    static constexpr uint32_t FIELD_COUNT = 256U * 256U;
    static constexpr uint32_t RIVER_INDEX = 1U;
    static constexpr uint32_t MAX_MUTED_TINT_CHANNEL_DELTA = 80U;

    generation::GenerationSnapshot terrain = makeSnapshot(generation::GenerationStage::Tectonics);
    std::optional<client::GenerationMapImage> base = client::rasterizeGenerationMap(terrain);
    ASSERT_TRUE(base);

    terrain.stage = generation::GenerationStage::InitialDrainage;
    terrain.water_level.assign(FIELD_COUNT, 0.5F);
    terrain.ocean_mask.assign(FIELD_COUNT, 0U);
    terrain.lake_mask.assign(FIELD_COUNT, 0U);
    terrain.river_mask.assign(FIELD_COUNT, 0U);
    terrain.river_mask[RIVER_INDEX] = 1U;

    std::optional<client::GenerationMapImage> drainage = client::rasterizeGenerationMap(terrain);

    ASSERT_TRUE(drainage);
    EXPECT_NE(drainage->rgba[RIVER_INDEX], base->rgba[RIVER_INDEX]);
    EXPECT_LT(
        maximumChannelDistance(drainage->rgba[RIVER_INDEX], base->rgba[RIVER_INDEX]),
        MAX_MUTED_TINT_CHANNEL_DELTA
    );
}

TEST(GenerationMapPresentationTest, MaximumResolutionIsBoundedAndAllPixelsAreOpaque)
{
    static constexpr uint32_t RESOLUTION = 512U;
    static constexpr uint32_t FIELD_COUNT = RESOLUTION * RESOLUTION;

    generation::GenerationSnapshot snapshot;
    snapshot.stage = generation::GenerationStage::Tectonics;
    snapshot.resolution = RESOLUTION;
    snapshot.elevation.assign(FIELD_COUNT, 1.0F);

    std::optional<client::GenerationMapImage> image = client::rasterizeGenerationMap(snapshot);

    ASSERT_TRUE(image);
    EXPECT_EQ(image->width, RESOLUTION);
    EXPECT_EQ(image->height, RESOLUTION);
    ASSERT_EQ(image->rgba.size(), FIELD_COUNT);
    EXPECT_TRUE(std::all_of(image->rgba.begin(), image->rgba.end(), [](Rgba8 const& pixel) {
        return pixel[3U] == 255U;
    }));
}

TEST(GenerationMapPresentationTest, AnimatorKeepsItsThirtySecondTimelineAfterRealReadiness)
{
    auto snapshot = std::make_shared<generation::GenerationSnapshot const>(
        makeSnapshot(generation::GenerationStage::Tectonics));
    auto final_image = std::make_shared<client::GenerationMapImage const>();
    client::GenerationMapAnimator animator;
    static constexpr std::array<client::GenerationMapAnimationPhase, 6U> PHASES{{
        client::GenerationMapAnimationPhase::Tectonics,
        client::GenerationMapAnimationPhase::Water,
        client::GenerationMapAnimationPhase::Relief,
        client::GenerationMapAnimationPhase::Ice,
        client::GenerationMapAnimationPhase::Climate,
        client::GenerationMapAnimationPhase::Arrival,
    }};
    static constexpr std::array<std::chrono::seconds, 6U> ELAPSED{{
        std::chrono::seconds{0},
        std::chrono::seconds{9},
        std::chrono::seconds{15},
        std::chrono::seconds{20},
        std::chrono::seconds{24},
        std::chrono::seconds{30},
    }};

    for (uint32_t index = 0U; index < static_cast<uint32_t>(PHASES.size()); ++index) {
        client::GenerationMapAnimationClock const clock{
            .visible_elapsed = ELAPSED[index],
            .formation_duration = std::chrono::seconds{20},
        };
        client::GenerationMapAnimationFrame const frame = animator.update(
            snapshot, nullptr, final_image, nullptr, clock);
        EXPECT_EQ(frame.phase, PHASES[index]);
        EXPECT_NE(frame.image, nullptr);
        if (index < 5U) {
            EXPECT_NE(frame.image, final_image);
        } else {
            EXPECT_EQ(frame.image, final_image);
        }
    }

    client::GenerationMapAnimationClock const ready_early_clock{
        .visible_elapsed = std::chrono::seconds{ 11U },
        .ready = true,
        .formation_duration = std::chrono::seconds{20},
    };
    client::GenerationMapAnimationFrame const ready_early = animator.update(
        snapshot, nullptr, final_image, nullptr, ready_early_clock);
    EXPECT_EQ(ready_early.phase, client::GenerationMapAnimationPhase::Water);
    EXPECT_NEAR(ready_early.phase_progress, 1.0F / 3.0F, 0.001F);
    EXPECT_NE(ready_early.image, final_image);

    client::GenerationMapAnimationClock const arrival_clock{
        .visible_elapsed = std::chrono::seconds{ 30U },
        .ready = true,
        .formation_duration = std::chrono::seconds{20},
    };
    client::GenerationMapAnimationFrame const arrival = animator.update(
        snapshot, nullptr, final_image, nullptr, arrival_clock);
    EXPECT_EQ(arrival.phase, client::GenerationMapAnimationPhase::Arrival);
    EXPECT_FLOAT_EQ(arrival.phase_progress, 1.0F);
    EXPECT_EQ(arrival.image, final_image);
}

TEST(GenerationMapPresentationTest, V21WaterPreviewUsesPhaseFieldsAndStockIce)
{
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr size_t FIELD_COUNT = static_cast<size_t>(RESOLUTION) * RESOLUTION;
    static constexpr size_t PHASE_FIELD_COUNT = FIELD_COUNT * 8U;
    auto snapshot = std::make_shared<generation::GenerationSnapshot>();
    snapshot->stage = generation::GenerationStage::V21Water;
    snapshot->resolution = RESOLUTION;
    generation::V21GenerationSnapshotFields& fields = snapshot->v21.emplace();
    fields.source_elevation_m.assign(FIELD_COUNT, 20.0F);
    fields.temperature_celsius.assign(FIELD_COUNT, 4.0F);
    fields.precipitation_mm_year.assign(FIELD_COUNT, 100.0F);
    fields.wind_u.assign(FIELD_COUNT, 0.5F);
    fields.wind_v.assign(FIELD_COUNT, 0.25F);
    fields.current_u.assign(FIELD_COUNT, 0.1F);
    fields.current_v.assign(FIELD_COUNT, -0.1F);
    fields.phase_temperature_celsius.assign(PHASE_FIELD_COUNT, -8.0F);
    fields.phase_precipitation_mm_year.assign(PHASE_FIELD_COUNT, 200.0F);
    fields.land_snow_water_equivalent_m.assign(PHASE_FIELD_COUNT, 0.0F);
    fields.glacier_thickness_m.assign(PHASE_FIELD_COUNT, 0.0F);
    fields.sea_ice_thickness_m.assign(PHASE_FIELD_COUNT, 0.0F);
    // V21 water levels are sea-relative game blocks; zero represents sea level.
    fields.water_level_blocks.assign(FIELD_COUNT, 0.0F);
    fields.ocean_mask.assign(FIELD_COUNT, 0U);
    fields.lake_mask.assign(FIELD_COUNT, 0U);
    fields.river_mask.assign(FIELD_COUNT, 0U);
    fields.biome_id.assign(FIELD_COUNT, 3U);
    fields.ice_surface_kind.assign(FIELD_COUNT, static_cast<uint8_t>(generation::V21GenerationIceSurfaceKind::None));
    fields.plate_ids.assign(FIELD_COUNT, 11U);
    fields.plate_directions = {{11U, 1.0F, 0.0F}};
    fields.plate_directions_observed_after_native_step = 3U;
    fields.land_snow_water_equivalent_m[100U] = 0.5F;
    fields.land_snow_water_equivalent_m[FIELD_COUNT + 101U] = 0.5F;
    fields.glacier_thickness_m[200U] = 1.0F;
    fields.glacier_thickness_m[FIELD_COUNT + 201U] = 1.0F;
    fields.ocean_mask[300U] = 1U;
    fields.ocean_mask[301U] = 1U;
    fields.sea_ice_thickness_m[300U] = 0.75F;
    fields.sea_ice_thickness_m[FIELD_COUNT + 301U] = 0.75F;
    fields.ice_surface_kind[100U] = static_cast<uint8_t>(generation::V21GenerationIceSurfaceKind::Snow);
    fields.ice_surface_kind[200U] = static_cast<uint8_t>(generation::V21GenerationIceSurfaceKind::GlacierIce);
    fields.ice_surface_kind[300U] = static_cast<uint8_t>(generation::V21GenerationIceSurfaceKind::SeaIce);
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*snapshot));

    auto const image = client::rasterizeGenerationMap(*snapshot);
    ASSERT_TRUE(image);
    EXPECT_EQ(image->width, RESOLUTION);
    auto no_stock_ice = std::make_shared<generation::GenerationSnapshot>(*snapshot);
    std::fill(no_stock_ice->v21->land_snow_water_equivalent_m.begin(),
        no_stock_ice->v21->land_snow_water_equivalent_m.end(), 0.0F);
    std::fill(no_stock_ice->v21->glacier_thickness_m.begin(), no_stock_ice->v21->glacier_thickness_m.end(), 0.0F);
    std::fill(no_stock_ice->v21->sea_ice_thickness_m.begin(), no_stock_ice->v21->sea_ice_thickness_m.end(), 0.0F);
    std::fill(no_stock_ice->v21->ice_surface_kind.begin(), no_stock_ice->v21->ice_surface_kind.end(),
        static_cast<uint8_t>(generation::V21GenerationIceSurfaceKind::None));
    auto const ice_free = client::rasterizeGenerationMap(*no_stock_ice);
    ASSERT_TRUE(ice_free);
    EXPECT_NE(image->rgba, ice_free->rgba);
    EXPECT_NE(image->rgba[100U], ice_free->rgba[100U]);
    EXPECT_NE(image->rgba[200U], ice_free->rgba[200U]);
    EXPECT_NE(image->rgba[300U], ice_free->rgba[300U]);
    EXPECT_NE(image->rgba[100U], image->rgba[200U]);
    EXPECT_NE(image->rgba[200U], image->rgba[300U]);
    EXPECT_EQ(image->rgba[101U], ice_free->rgba[101U]);
    EXPECT_EQ(image->rgba[201U], ice_free->rgba[201U]);
    EXPECT_EQ(image->rgba[301U], ice_free->rgba[301U]);

    std::vector<uint8_t> const proxy_mask(FIELD_COUNT, client::GENERATION_MAP_ICE_NONE);
    auto const ignored_proxy = client::rasterizeGenerationMap(*snapshot, proxy_mask);
    ASSERT_TRUE(ignored_proxy);
    EXPECT_EQ(image->rgba, ignored_proxy->rgba);
    client::GenerationMapAnimator animator;
    auto const frame = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{0}});
    ASSERT_NE(frame.image, nullptr);
    EXPECT_EQ(frame.phase, client::GenerationMapAnimationPhase::Creation);

    // Ice uses phase zero even when the other seasons keep the annual mean fixed.
    auto warm_phase_zero = std::make_shared<generation::GenerationSnapshot>(*snapshot);
    warm_phase_zero->v21->phase_temperature_celsius[101U] = 20.0F;
    warm_phase_zero->v21->phase_temperature_celsius[FIELD_COUNT + 101U] = -36.0F;
    client::GenerationMapAnimator cold_animator;
    client::GenerationMapAnimator warm_animator;
    client::GenerationMapAnimationClock const peak_clock{
        .visible_elapsed = std::chrono::milliseconds{11'250},
        .formation_duration = std::chrono::seconds{10},
    };
    auto cold_peak = cold_animator.update(snapshot, nullptr, nullptr, nullptr, peak_clock);
    auto warm_peak = warm_animator.update(warm_phase_zero, nullptr, nullptr, nullptr, peak_clock);
    uint32_t const ice_pixel = 101U * client::GENERATION_MAP_ANIMATION_SCALE;
    EXPECT_NE(cold_peak.image->rgba[ice_pixel], warm_peak.image->rgba[ice_pixel]);
}

TEST(GenerationMapPresentationTest, V21OceansRiseAfterCoolingDuringWaterPhase)
{
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr size_t FIELD_COUNT = static_cast<size_t>(RESOLUTION) * RESOLUTION;
    static constexpr size_t PHASE_FIELD_COUNT = FIELD_COUNT * 8U;
    auto snapshot = std::make_shared<generation::GenerationSnapshot>();
    snapshot->stage = generation::GenerationStage::V21Water;
    snapshot->resolution = RESOLUTION;
    generation::V21GenerationSnapshotFields& fields = snapshot->v21.emplace();
    fields.source_elevation_m.resize(FIELD_COUNT);
    for (uint32_t y = 0U; y < RESOLUTION; ++y)
    {
        for (uint32_t x = 0U; x < RESOLUTION; ++x)
        {
            fields.source_elevation_m[static_cast<size_t>(y) * RESOLUTION + x]
                = -1'000.0F + 1'000.0F * static_cast<float>(x) / static_cast<float>(RESOLUTION - 1U);
        }
    }
    fields.temperature_celsius.assign(FIELD_COUNT, 10.0F);
    fields.precipitation_mm_year.assign(FIELD_COUNT, 100.0F);
    fields.wind_u.assign(FIELD_COUNT, 0.0F);
    fields.wind_v.assign(FIELD_COUNT, 0.0F);
    fields.current_u.assign(FIELD_COUNT, 0.0F);
    fields.current_v.assign(FIELD_COUNT, 0.0F);
    fields.phase_temperature_celsius.assign(PHASE_FIELD_COUNT, 10.0F);
    fields.phase_precipitation_mm_year.assign(PHASE_FIELD_COUNT, 100.0F);
    fields.land_snow_water_equivalent_m.assign(PHASE_FIELD_COUNT, 0.0F);
    fields.glacier_thickness_m.assign(PHASE_FIELD_COUNT, 0.0F);
    fields.sea_ice_thickness_m.assign(PHASE_FIELD_COUNT, 0.0F);
    fields.water_level_blocks.assign(FIELD_COUNT, 0.0F);
    fields.ocean_mask.assign(FIELD_COUNT, 1U);
    fields.lake_mask.assign(FIELD_COUNT, 0U);
    fields.river_mask.assign(FIELD_COUNT, 0U);
    fields.biome_id.assign(FIELD_COUNT, 1U);
    fields.ice_surface_kind.assign(FIELD_COUNT, static_cast<uint8_t>(generation::V21GenerationIceSurfaceKind::None));
    fields.plate_ids.assign(FIELD_COUNT, 1U);
    fields.plate_directions = {{1U, 0.0F, 0.0F}};
    fields.plate_directions_observed_after_native_step = 1U;
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*snapshot));

    client::GenerationMapAnimator animator;
    auto cooling = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{1'000}, .formation_duration = std::chrono::seconds{10}});
    auto cooling_late = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{2'999}, .formation_duration = std::chrono::seconds{10}});
    auto water_start = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{3'000}, .formation_duration = std::chrono::seconds{10}});
    auto water_early = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{3'300}, .formation_duration = std::chrono::seconds{10}});
    auto water_late = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{4'800}, .formation_duration = std::chrono::seconds{10}});
    auto water_end = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{5'249}, .formation_duration = std::chrono::seconds{10}});
    auto drift_start = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{5'250}, .formation_duration = std::chrono::seconds{10}});
    ASSERT_NE(cooling.image, nullptr);
    ASSERT_NE(cooling_late.image, nullptr);
    ASSERT_NE(water_start.image, nullptr);
    ASSERT_NE(water_early.image, nullptr);
    ASSERT_NE(water_late.image, nullptr);
    ASSERT_NE(water_end.image, nullptr);
    ASSERT_NE(drift_start.image, nullptr);
    auto count_dominant_blue_pixels = [](auto const& image) {
        return std::count_if(image->rgba.begin(), image->rgba.end(), [](Rgba8 const& pixel) {
            return pixel[2U] > pixel[0U] && pixel[2U] > pixel[1U];
        });
    };
    EXPECT_EQ(count_dominant_blue_pixels(cooling.image), 0U);
    EXPECT_EQ(count_dominant_blue_pixels(cooling_late.image), 0U);
    EXPECT_TRUE(cooling_late.image->rgba == water_start.image->rgba);
    EXPECT_LT(count_dominant_blue_pixels(water_early.image), count_dominant_blue_pixels(water_late.image));
    EXPECT_GT(count_dominant_blue_pixels(water_late.image), 0U);
    EXPECT_TRUE(water_end.image->rgba == drift_start.image->rgba);
}

TEST(GenerationMapPresentationTest, V21ClimatePhaseFadesIntoFinalBiomeMap)
{
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr size_t FIELD_COUNT = static_cast<size_t>(RESOLUTION) * RESOLUTION;
    static constexpr size_t PHASE_FIELD_COUNT = FIELD_COUNT * 8U;
    auto snapshot = std::make_shared<generation::GenerationSnapshot>();
    snapshot->stage = generation::GenerationStage::V21Water;
    snapshot->resolution = RESOLUTION;
    generation::V21GenerationSnapshotFields& fields = snapshot->v21.emplace();
    fields.source_elevation_m.assign(FIELD_COUNT, 0.0F);
    fields.temperature_celsius.assign(FIELD_COUNT, 12.0F);
    fields.precipitation_mm_year.assign(FIELD_COUNT, 100.0F);
    fields.wind_u.assign(FIELD_COUNT, 0.0F);
    fields.wind_v.assign(FIELD_COUNT, 0.0F);
    fields.current_u.assign(FIELD_COUNT, 0.0F);
    fields.current_v.assign(FIELD_COUNT, 0.0F);
    fields.phase_temperature_celsius.assign(PHASE_FIELD_COUNT, 12.0F);
    fields.phase_precipitation_mm_year.assign(PHASE_FIELD_COUNT, 100.0F);
    fields.land_snow_water_equivalent_m.assign(PHASE_FIELD_COUNT, 0.0F);
    fields.glacier_thickness_m.assign(PHASE_FIELD_COUNT, 0.0F);
    fields.sea_ice_thickness_m.assign(PHASE_FIELD_COUNT, 0.0F);
    fields.water_level_blocks.assign(FIELD_COUNT, 0.0F);
    fields.ocean_mask.assign(FIELD_COUNT, 0U);
    fields.lake_mask.assign(FIELD_COUNT, 0U);
    fields.river_mask.assign(FIELD_COUNT, 0U);
    fields.biome_id.assign(FIELD_COUNT, 1U);
    fields.ice_surface_kind.assign(FIELD_COUNT, static_cast<uint8_t>(generation::V21GenerationIceSurfaceKind::None));
    fields.plate_ids.assign(FIELD_COUNT, 1U);
    fields.plate_directions = {{1U, 0.0F, 0.0F}};
    fields.plate_directions_observed_after_native_step = 1U;
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*snapshot));

    auto final_image = std::make_shared<client::GenerationMapImage>();
    final_image->width = client::GENERATION_MAP_ANIMATION_RENDER_RESOLUTION;
    final_image->height = client::GENERATION_MAP_ANIMATION_RENDER_RESOLUTION;
    final_image->rgba.resize(static_cast<size_t>(final_image->width) * final_image->height);
    for (uint32_t y = 0U; y < final_image->height; ++y)
    {
        for (uint32_t x = 0U; x < final_image->width; ++x)
        {
            Rgba8 color{};
            if (x < final_image->width / 2U && y < final_image->height / 2U)
            {
                color = {118U, 154U, 92U, 255U}; // biome land
            }
            else if (x >= final_image->width / 2U && y < final_image->height / 2U)
            {
                color = {218U, 190U, 126U, 255U}; // desert
            }
            else if (x < final_image->width / 2U)
            {
                color = {66U, 126U, 164U, 255U}; // ocean
            }
            else
            {
                color = {224U, 231U, 231U, 255U}; // snow
            }
            final_image->rgba[static_cast<size_t>(y) * final_image->width + x] = color;
        }
    }

    client::GenerationMapAnimator animator;
    auto const climate_start = animator.update(snapshot, nullptr, final_image, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{13'500}, .formation_duration = std::chrono::seconds{10}});
    auto const climate_mid = animator.update(snapshot, nullptr, final_image, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{14'250}, .formation_duration = std::chrono::seconds{10}});
    auto const climate_late = animator.update(snapshot, nullptr, final_image, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{14'750}, .formation_duration = std::chrono::seconds{10}});
    auto const climate_end = animator.update(snapshot, nullptr, final_image, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{14'999}, .formation_duration = std::chrono::seconds{10}});
    auto const arrival = animator.update(snapshot, nullptr, final_image, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{15'000}, .formation_duration = std::chrono::seconds{10}});
    ASSERT_NE(climate_start.image, nullptr);
    ASSERT_NE(climate_mid.image, nullptr);
    ASSERT_NE(climate_late.image, nullptr);
    ASSERT_NE(climate_end.image, nullptr);
    ASSERT_NE(arrival.image, nullptr);
    auto const mean_distance_to_final = [&final_image](client::GenerationMapImageHandle const& image) {
        uint64_t total_distance = 0U;
        for (size_t pixel = 0U; pixel < image->rgba.size(); ++pixel)
        {
            for (size_t channel = 0U; channel < 3U; ++channel)
            {
                int const difference = static_cast<int>(image->rgba[pixel][channel])
                    - static_cast<int>(final_image->rgba[pixel][channel]);
                total_distance += static_cast<uint64_t>(std::abs(difference));
            }
        }
        return static_cast<double>(total_distance)
            / static_cast<double>(image->rgba.size() * 3U);
    };
    EXPECT_EQ(climate_mid.phase, client::GenerationMapAnimationPhase::Climate);
    EXPECT_LT(mean_distance_to_final(climate_mid.image), mean_distance_to_final(climate_start.image));
    EXPECT_LT(mean_distance_to_final(climate_late.image), mean_distance_to_final(climate_mid.image));
    EXPECT_LT(mean_distance_to_final(climate_late.image), 8.0);
    EXPECT_TRUE(climate_end.image->rgba == final_image->rgba);
    EXPECT_EQ(arrival.image->rgba, final_image->rgba);
}

TEST(GenerationMapPresentationTest, V21ClimatePreviewAllowsOptionalPrecipitationAndCurrents)
{
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr uint32_t FIELD_COUNT = RESOLUTION * RESOLUTION;
    static constexpr uint32_t PHASE_FIELD_COUNT = FIELD_COUNT * 8U;
    static constexpr uint32_t COLD_CELL = 0U;
    static constexpr uint32_t WARM_CELL = 1U;

    auto snapshot = std::make_shared<generation::GenerationSnapshot>();
    snapshot->stage = generation::GenerationStage::V21Climate;
    snapshot->resolution = RESOLUTION;
    generation::V21GenerationSnapshotFields& fields = snapshot->v21.emplace();
    fields.source_elevation_m.assign(FIELD_COUNT, 20.0F);
    fields.temperature_celsius.assign(FIELD_COUNT, 4.0F);
    fields.wind_u.assign(FIELD_COUNT, 0.25F);
    fields.wind_v.assign(FIELD_COUNT, -0.1F);
    fields.phase_temperature_celsius.assign(PHASE_FIELD_COUNT, 4.0F);
    fields.plate_ids.assign(FIELD_COUNT, 11U);
    fields.plate_directions = {{11U, 1.0F, 0.0F}};
    fields.plate_directions_observed_after_native_step = 3U;
    fields.temperature_celsius[COLD_CELL] = -18.0F;
    fields.temperature_celsius[WARM_CELL] = 18.0F;
    for (uint32_t phase = 0U; phase < 8U; ++phase)
    {
        fields.phase_temperature_celsius[phase * FIELD_COUNT + COLD_CELL] = -18.0F;
        fields.phase_temperature_celsius[phase * FIELD_COUNT + WARM_CELL] = 18.0F;
    }
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*snapshot));

    auto const image = client::rasterizeGenerationMap(*snapshot);
    ASSERT_TRUE(image);
    EXPECT_NE(image->rgba[COLD_CELL], image->rgba[WARM_CELL]);

    client::GenerationMapAnimator animator;
    auto const frame = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{0}});
    ASSERT_NE(frame.image, nullptr);
    EXPECT_EQ(frame.phase, client::GenerationMapAnimationPhase::Creation);
}

TEST(GenerationMapPresentationTest, V9PlatecPreviewRendersSourceReliefAndPlateDrift)
{
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr uint32_t FIELD_COUNT = RESOLUTION * RESOLUTION;
    auto snapshot = std::make_shared<generation::GenerationSnapshot>();
    snapshot->stage = generation::GenerationStage::V9Platec;
    snapshot->resolution = RESOLUTION;
    snapshot->v9.emplace();
    snapshot->v9->source_elevation_m.resize(FIELD_COUNT);
    snapshot->v9->plate_ids.resize(FIELD_COUNT);
    for (uint32_t y = 0U; y < RESOLUTION; ++y)
    {
        for (uint32_t x = 0U; x < RESOLUTION; ++x)
        {
            size_t const index = static_cast<size_t>(y) * RESOLUTION + x;
            snapshot->v9->source_elevation_m[index] = (static_cast<float>(x) - 128.0F) * 30.0F;
            snapshot->v9->plate_ids[index] = x < RESOLUTION / 2U ? 11U : 42U;
        }
    }
    snapshot->v9->plate_directions = {{11U, 1.0F, 0.0F}, {42U, -1.0F, 0.0F}};
    snapshot->v9->plate_directions_observed_after_native_step = 2'399U;
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*snapshot));

    auto const immutable_snapshot = std::make_shared<generation::GenerationSnapshot const>(*snapshot);
    client::GenerationMapAnimator animator;
    client::GenerationMapAnimationFrame const initial = animator.update(
        immutable_snapshot,
        nullptr,
        nullptr,
        nullptr,
        {.visible_elapsed = std::chrono::milliseconds{0}}
    );
    client::GenerationMapAnimationFrame const drifting = animator.update(
        immutable_snapshot,
        nullptr,
        nullptr,
        nullptr,
        {.visible_elapsed = std::chrono::milliseconds{2'500}}
    );

    ASSERT_NE(initial.image, nullptr);
    ASSERT_NE(drifting.image, nullptr);
    EXPECT_EQ(initial.phase, client::GenerationMapAnimationPhase::Creation);
    EXPECT_NE(initial.image->rgba, drifting.image->rgba);
    auto atlas = std::make_shared<generation::GlobalAtlas>();
    atlas->configuration.model_tag = generation::TerrainModelTag::V9;
    EXPECT_TRUE(atlas->elevation.empty());
    auto with_atlas = animator.update(immutable_snapshot, atlas, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{2'500}});
    auto same_tick = animator.update(immutable_snapshot, atlas, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{2'501}});
    EXPECT_EQ(with_atlas.image, same_tick.image);
}

TEST(GenerationMapPresentationTest, ReusesOnlyLatestVisualTickAndInvalidatesChangedInputs)
{
    auto snapshot = std::make_shared<generation::GenerationSnapshot const>(
        makeSnapshot(generation::GenerationStage::Tectonics));
    client::GenerationMapAnimator animator;
    auto render = [&](auto const& source, auto const& mask, int64_t const elapsed) {
        return animator.update(source, nullptr, nullptr, mask,
            {.visible_elapsed = std::chrono::milliseconds{elapsed}}).image;
    };
    client::GenerationMapDisplayIceMaskHandle const no_mask;
    auto first = render(snapshot, no_mask, 750);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first, render(snapshot, no_mask, 751));
    auto changed_tick = render(snapshot, no_mask, 1'000);
    EXPECT_NE(first, changed_tick);
    auto changed_phase = render(snapshot, no_mask, 5'750);
    EXPECT_NE(changed_tick, changed_phase);
    auto changed_source = std::make_shared<generation::GenerationSnapshot const>(*snapshot);
    auto source_frame = render(changed_source, no_mask, 5'750);
    EXPECT_NE(changed_phase, source_frame);
    auto mask = std::make_shared<client::GenerationMapDisplayIceMask const>();
    auto mask_frame = render(changed_source, mask, 5'750);
    EXPECT_NE(source_frame, mask_frame);
    EXPECT_EQ(mask_frame, render(changed_source, mask, 5'751));
    animator.reset();
    EXPECT_NE(mask_frame, render(changed_source, mask, 5'751));

    std::weak_ptr<client::GenerationMapImage const> previous;
    {
        auto frame = render(snapshot, no_mask, 750);
        previous = frame;
    }
    ASSERT_FALSE(previous.expired());
    static_cast<void>(render(snapshot, no_mask, 1'000));
    EXPECT_TRUE(previous.expired());
    auto atlas = makeDriftingMapAtlas();
    auto atlas_frame = animator.update(nullptr, atlas, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{750}});
    EXPECT_EQ(atlas_frame.image, animator.update(nullptr, atlas, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{751}}).image);
    auto changed_atlas = std::make_shared<generation::GlobalAtlas const>(*atlas);
    EXPECT_NE(atlas_frame.image, animator.update(nullptr, changed_atlas, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{751}}).image);
}

TEST(GenerationMapPresentationTest, DelayedNativeWaterShowsProgressiveIceAndWeatherBeforeExactArrival)
{
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr uint32_t COUNT = RESOLUTION * RESOLUTION;
    auto water = std::make_shared<generation::GenerationSnapshot>();
    water->stage = generation::GenerationStage::V9Water;
    water->resolution = RESOLUTION;
    water->v9.emplace();
    auto& fields = *water->v9;
    fields.source_elevation_m.assign(COUNT, 500.0F);
    fields.plate_ids.assign(COUNT, 1U);
    fields.plate_directions = {{1U, 1.0F, 0.0F}};
    fields.temperature_celsius.resize(COUNT);
    fields.precipitation_mm_year.assign(COUNT, 500.0F);
    fields.water_level_m.assign(COUNT, 0.0F);
    fields.ocean_mask.assign(COUNT, 0U);
    fields.lake_mask.assign(COUNT, 0U);
    fields.river_mask.assign(COUNT, 0U);
    fields.ice_surface_kind.resize(COUNT);
    for (uint32_t index = 0U; index < COUNT; ++index)
    {
        uint32_t const x = index % RESOLUTION;
        fields.temperature_celsius[index] = -20.0F + static_cast<float>(x) / 6.0F;
        fields.ice_surface_kind[index] = x < 120U ? client::GENERATION_MAP_ICE_SNOW : client::GENERATION_MAP_ICE_NONE;
    }
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*water));
    auto early = std::make_shared<generation::GenerationSnapshot>(*water);
    early->stage = generation::GenerationStage::V9Platec;
    early->v9->temperature_celsius.clear();
    early->v9->precipitation_mm_year.clear();
    early->v9->water_level_m.clear();
    early->v9->ocean_mask.clear();
    early->v9->lake_mask.clear();
    early->v9->river_mask.clear();
    early->v9->ice_surface_kind.clear();
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*early));
    auto climate = std::make_shared<generation::GenerationSnapshot>(*early);
    climate->stage = generation::GenerationStage::V9Climate;
    climate->v9->temperature_celsius = fields.temperature_celsius;
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*climate));
    auto final_pixels = client::rasterizeGenerationMap(*water, fields.ice_surface_kind);
    ASSERT_TRUE(final_pixels);
    auto final_image = std::make_shared<client::GenerationMapImage const>(std::move(*final_pixels));
    client::GenerationMapAnimator animator;
    static_cast<void>(animator.update(early, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{750}, .formation_duration = std::chrono::seconds{20}}));
    static_cast<void>(animator.update(climate, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{5'834}, .formation_duration = std::chrono::seconds{20}}));
    auto render = [&](int64_t const elapsed) {
        return animator.update(water, nullptr, final_image, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{elapsed}, .ready = true,
             .formation_duration = std::chrono::seconds{20}});
    };
    EXPECT_EQ(render(31'000).phase, client::GenerationMapAnimationPhase::Creation);
    auto filling = render(35'000);
    ASSERT_EQ(filling.phase, client::GenerationMapAnimationPhase::Water);
    EXPECT_NE(filling.image, final_image);
    EXPECT_EQ(render(35'015).image, filling.image);
    EXPECT_NE(render(35'050).image, filling.image);
    EXPECT_EQ(render(41'000).phase, client::GenerationMapAnimationPhase::Relief);
    auto ice_start = render(45'000);
    ASSERT_EQ(ice_start.phase, client::GenerationMapAnimationPhase::Ice);
    auto const canonical_base = client::s2NativeTerrainColor(500.0F, {500.0F, 500.0F}, {500.0F, 500.0F}, false, 0.0F, 0U);
    Rgba8 expected_base{0U, 0U, 0U, 255U};
    for (uint32_t channel = 0U; channel < 3U; ++channel)
    {
        expected_base[channel] = static_cast<uint8_t>(std::lround(canonical_base[channel]));
    }
    client::GenerationMapAnimator endpoint_animator;
    auto const water_endpoint = endpoint_animator.update(water, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{17'999}, .formation_duration = std::chrono::seconds{20}});
    EXPECT_TRUE(std::all_of(water_endpoint.image->rgba.begin(), water_endpoint.image->rgba.end(),
        [&](Rgba8 const& pixel) { return pixel == expected_base; }));
    auto ice_early = render(45'500);
    auto ice_late = render(49'300);
    auto frozen_count = [&](auto const& image) {
        return std::count_if(image->rgba.begin(), image->rgba.end(), [&](Rgba8 const& pixel) {
            return maximumChannelDistance(pixel, expected_base) > 12U;
        });
    };
    EXPECT_GT(frozen_count(ice_late.image), frozen_count(ice_early.image));
    auto retreat = render(53'000);
    auto climate_start = render(54'000);
    EXPECT_EQ(climate_start.phase, client::GenerationMapAnimationPhase::Climate);
    auto weather = render(55'500);
    EXPECT_EQ(weather.phase, client::GenerationMapAnimationPhase::Climate);
    EXPECT_NE(weather.image->rgba, climate_start.image->rgba);
    auto frozen_fraction = [&](auto const& image) {
        return static_cast<double>(frozen_count(image)) / static_cast<double>(image->rgba.size());
    };
    EXPECT_GT(frozen_fraction(ice_late.image), frozen_fraction(retreat.image));
    uint32_t const snow_pixel = animationRenderPixelIndex(60U, 128U);
    uint32_t const final_snow_pixel = 128U * final_image->width + 60U;
    EXPECT_LE(
        maximumChannelDistance(weather.image->rgba[snow_pixel], final_image->rgba[final_snow_pixel]),
        40U
    );
    auto arrival = render(57'000);
    EXPECT_EQ(arrival.phase, client::GenerationMapAnimationPhase::Arrival);
    EXPECT_EQ(arrival.image, final_image);
    EXPECT_TRUE(writePpmIfRequested("native-generation-water.ppm", *filling.image));
    EXPECT_TRUE(writePpmIfRequested("native-generation-ice-growth.ppm", *ice_early.image));
    EXPECT_TRUE(writePpmIfRequested("native-generation-ice-age-peak.ppm", *ice_late.image));
    EXPECT_TRUE(writePpmIfRequested("native-generation-climate-retreat.ppm", *weather.image));
    EXPECT_TRUE(writePpmIfRequested("native-generation-arrival.ppm", *arrival.image));
}

TEST(GenerationMapPresentationTest, MovingNearestSamplesKeepSourceColorsWithoutPhaseOnlyRgbBlur)
{
    auto atlas = std::make_shared<generation::GlobalAtlas>(*makeDriftingMapAtlas());
    uint32_t const count = atlas->configuration.resolution * atlas->configuration.resolution;
    atlas->water_level.assign(count, 0.0F);
    atlas->ocean_mask.resize(count);
    for (uint32_t index = 0U; index < count; ++index)
    {
        atlas->ocean_mask[index] = atlas->elevation[index] < 0.0F ? 1U : 0U;
    }
    atlas->lake_mask.assign(count, 0U);
    atlas->river_mask.assign(count, 0U);
    atlas->temperature.assign(count, 15.0F);
    atlas->precipitation.assign(count, 0.0F);
    auto const final_source_image = client::rasterizeGenerationMap(*atlas, {}, false);
    ASSERT_TRUE(final_source_image);
    static constexpr uint32_t INTERIOR_LAND_PIXEL = 160U;
    static constexpr uint32_t INTERIOR_WATER_PIXEL = 220U;
    static constexpr uint32_t SAMPLE_Y = 100U;
    static constexpr Rgba8 LAND_SOURCE_COLOR{229U, 231U, 224U, 255U};
    static constexpr Rgba8 WATER_SOURCE_COLOR{35U, 72U, 94U, 255U};
    client::GenerationMapAnimator animator;
    for (int64_t const elapsed : {0, 18'750, 21'500, 24'000})
    {
        auto const frame = animator.update(nullptr, atlas, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{elapsed}});
        ASSERT_NE(frame.image, nullptr);
        EXPECT_EQ(frame.image->rgba[animationRenderPixelIndex(INTERIOR_LAND_PIXEL, SAMPLE_Y)], LAND_SOURCE_COLOR);
        Rgba8 const expected_water = frame.phase == client::GenerationMapAnimationPhase::Tectonics
            ? WATER_SOURCE_COLOR
            : final_source_image->rgba[SAMPLE_Y * final_source_image->width + INTERIOR_WATER_PIXEL];
        EXPECT_EQ(frame.image->rgba[animationRenderPixelIndex(INTERIOR_WATER_PIXEL, SAMPLE_Y)], expected_water);
        if (elapsed == 21'500)
        {
            EXPECT_EQ(frame.phase, client::GenerationMapAnimationPhase::Relief);
            EXPECT_NEAR(frame.phase_progress, 0.49F, 0.01F);
        }
    }
}

std::shared_ptr<generation::GenerationSnapshot> makeEngagementSnapshot()
{
    auto snapshot = std::make_shared<generation::GenerationSnapshot>();
    snapshot->stage = generation::GenerationStage::V9Water;
    snapshot->resolution = 256U;
    auto& fields = snapshot->v9.emplace();
    for (uint32_t y = 0U; y < 256U; ++y)
    {
        for (uint32_t x = 0U; x < 256U; ++x)
        {
            float const height = 900.0F * std::sin(static_cast<float>(x) * 0.049F)
                * std::cos(static_cast<float>(y) * 0.049F);
            fields.source_elevation_m.push_back(height);
            fields.plate_ids.push_back(x < 128U ? 1U : 2U);
            fields.temperature_celsius.push_back(15.0F);
            fields.precipitation_mm_year.push_back(500.0F);
            fields.water_level_m.push_back(0.0F);
            fields.ocean_mask.push_back(height < 0.0F ? 1U : 0U);
        }
    }
    fields.plate_directions = {{1U, 1.0F, 0.0F}, {2U, -1.0F, 0.0F}};
    fields.lake_mask.assign(256U * 256U, 0U);
    fields.river_mask = fields.lake_mask;
    fields.ice_surface_kind = fields.lake_mask;
    fields.wind_u.assign(256U * 256U, 1.0F);
    fields.wind_v.assign(256U * 256U, 0.0F);
    fields.current_u.assign(256U * 256U, 0.4F);
    fields.current_v.assign(256U * 256U, 0.2F);
    return snapshot;
}

TEST(GenerationMapPresentationTest, NativeCreationWaitsForRainThenPlaysOneCompleteMonotonicSequence)
{
    auto complete = makeEngagementSnapshot();
    auto dry = std::make_shared<generation::GenerationSnapshot>(*complete);
    dry->stage = generation::GenerationStage::V9Climate;
    dry->v9->precipitation_mm_year.clear();
    dry->v9->ice_surface_kind.clear();
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*dry));
    client::GenerationMapAnimator animator;
    auto render = [&](auto const& source, int64_t const milliseconds) {
        return animator.update(source, nullptr, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{milliseconds},
             .formation_duration = std::chrono::seconds{10}});
    };
    static_cast<void>(render(dry, 0));
    auto cooling = render(dry, 6'000);
    auto waiting = render(dry, 12'000);
    EXPECT_EQ(waiting.phase, client::GenerationMapAnimationPhase::Creation);
    EXPECT_LT(waiting.phase_progress, 0.75F);
    EXPECT_GT(waiting.phase_progress, cooling.phase_progress);
    EXPECT_NE(waiting.image->rgba, cooling.image->rgba);
    auto arriving = render(complete, 12'000);
    EXPECT_EQ(arriving.phase, client::GenerationMapAnimationPhase::Creation);
    ASSERT_NE(waiting.image, nullptr);
    ASSERT_NE(arriving.image, nullptr);
    EXPECT_EQ(waiting.image->rgba, arriving.image->rgba);
    auto raining = render(complete, 13'500);
    EXPECT_EQ(raining.phase, client::GenerationMapAnimationPhase::Water);
    EXPECT_GT(raining.phase_progress, 0.0F);
    EXPECT_LT(raining.phase_progress, 0.5F);
    EXPECT_NE(raining.image->rgba, arriving.image->rgba);
    EXPECT_EQ(render(complete, 16'500).phase, client::GenerationMapAnimationPhase::Relief);
    EXPECT_EQ(render(complete, 19'500).phase, client::GenerationMapAnimationPhase::Ice);
    EXPECT_EQ(render(complete, 23'500).phase, client::GenerationMapAnimationPhase::Climate);
    EXPECT_EQ(render(complete, 25'000).phase, client::GenerationMapAnimationPhase::Arrival);
}

TEST(GenerationMapPresentationTest, NativeSourceReplacementKeepsCurrentImageAndPhase)
{
    auto snapshot = makeEngagementSnapshot();
    client::GenerationMapAnimator animator;
    auto render = [&](auto const& source, int64_t const milliseconds) {
        return animator.update(source, nullptr, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{milliseconds},
             .formation_duration = std::chrono::seconds{10}});
    };
    auto before = render(snapshot, 10'500);
    ASSERT_EQ(before.phase, client::GenerationMapAnimationPhase::Ice);
    auto replacement = std::make_shared<generation::GenerationSnapshot>(*snapshot);
    std::fill(replacement->v9->source_elevation_m.begin(), replacement->v9->source_elevation_m.end(), 500.0F);
    auto after = render(replacement, 10'500);
    EXPECT_EQ(after.phase, before.phase);
    EXPECT_FLOAT_EQ(after.phase_progress, before.phase_progress);
    ASSERT_NE(before.image, nullptr);
    ASSERT_NE(after.image, nullptr);
    EXPECT_EQ(before.image->rgba, after.image->rgba);
    EXPECT_NE(render(replacement, 10'800).image->rgba, before.image->rgba);
}

TEST(GenerationMapPresentationTest, ZeroDurationCreationStillResumesAfterWaitingForData)
{
    auto complete = makeEngagementSnapshot();
    auto early = std::make_shared<generation::GenerationSnapshot>(*complete);
    early->stage = generation::GenerationStage::V9Climate;
    early->v9->precipitation_mm_year.clear();
    early->v9->ice_surface_kind.clear();
    client::GenerationMapAnimator animator;
    auto render = [&](auto const& source, int64_t const milliseconds) {
        return animator.update(source, nullptr, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{milliseconds},
             .formation_duration = std::chrono::milliseconds{0}});
    };
    static_cast<void>(render(early, 0));
    std::feclearexcept(FE_INVALID | FE_DIVBYZERO);
    EXPECT_EQ(render(early, 10).phase, client::GenerationMapAnimationPhase::Creation);
    EXPECT_EQ(std::fetestexcept(FE_INVALID | FE_DIVBYZERO), 0);
    static_cast<void>(render(complete, 11));
    EXPECT_EQ(render(complete, 12).phase, client::GenerationMapAnimationPhase::Arrival);
}

TEST(GenerationMapPresentationTest, NativeIceAgeAdvancesFromStockIceAndRetreatsBeforeClimate)
{
    auto snapshot = makeEngagementSnapshot();
    auto& fields = *snapshot->v9;
    std::fill(fields.source_elevation_m.begin(), fields.source_elevation_m.end(), 500.0F);
    std::fill(fields.ocean_mask.begin(), fields.ocean_mask.end(), 0U);
    std::fill(fields.temperature_celsius.begin(), fields.temperature_celsius.end(), 5.0F);
    fields.plate_directions = {{1U, 0.0F, 0.0F}, {2U, 0.0F, 0.0F}};
    for (uint32_t y = 0U; y < 256U; ++y)
    {
        fields.ice_surface_kind[y * 256U] = client::GENERATION_MAP_ICE_SNOW;
    }
    client::GenerationMapAnimator animator;
    auto render = [&](int64_t const milliseconds) {
        return animator.update(snapshot, nullptr, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{milliseconds},
             .formation_duration = std::chrono::seconds{10}});
    };
    auto before = render(9'000);
    auto early = render(9'562);
    auto peak = render(11'250);
    auto retreating = render(12'937);
    auto settled = render(13'500);
    auto frozen = [&](auto const& image, uint32_t const index) {
        return maximumChannelDistance(image->rgba[index], before.image->rgba[index]) > 12U;
    };
    auto frozenCount = [&](auto const& image) {
        size_t count = 0U;
        for (uint32_t index = 0U; index < image->rgba.size(); ++index)
        {
            count += frozen(image, index) ? 1U : 0U;
        }
        return count;
    };
    uint32_t const near_seed = animationRenderPixelIndex(4U, 128U);
    uint32_t const early_change = maximumChannelDistance(early.image->rgba[near_seed], before.image->rgba[near_seed]);
    EXPECT_GT(early_change, 1U);
    EXPECT_LT(early_change, maximumChannelDistance(peak.image->rgba[near_seed], before.image->rgba[near_seed]));
    EXPECT_FALSE(frozen(early.image, animationRenderPixelIndex(64U, 128U)));
    EXPECT_GT(frozenCount(peak.image), frozenCount(early.image));
    EXPECT_GT(frozenCount(peak.image), peak.image->rgba.size() / 2U);
    EXPECT_LT(frozenCount(retreating.image), frozenCount(peak.image));
    EXPECT_FALSE(frozen(settled.image, animationRenderPixelIndex(64U, 128U)));
}

TEST(GenerationMapPresentationTest, WaterPhaseRainRemainsVisibleAtMapScale)
{
    auto rainy = makeEngagementSnapshot();
    auto dry = std::make_shared<generation::GenerationSnapshot>(*rainy);
    std::fill(dry->v9->precipitation_mm_year.begin(), dry->v9->precipitation_mm_year.end(), 0.0F);
    client::GenerationMapAnimator rainy_animator;
    client::GenerationMapAnimator dry_animator;
    client::GenerationMapAnimationClock const clock{
        .visible_elapsed = std::chrono::milliseconds{4'125},
        .formation_duration = std::chrono::seconds{10},
    };
    auto rain = rainy_animator.update(rainy, nullptr, nullptr, nullptr, clock);
    auto clear = dry_animator.update(dry, nullptr, nullptr, nullptr, clock);
    size_t changed = 0U;
    for (size_t index = 0U; index < rain.image->rgba.size(); ++index)
    {
        changed += rain.image->rgba[index] != clear.image->rgba[index] ? 1U : 0U;
    }
    EXPECT_GT(changed, rain.image->rgba.size() / 200U);
}

TEST(GenerationMapPresentationTest, RainRetainsWeakerWetRegionsBesideAWetterContinent)
{
    auto rainy = makeEngagementSnapshot();
    auto& fields = *rainy->v9;
    fields.source_elevation_m.assign(256U * 256U, 500.0F);
    fields.ocean_mask.assign(256U * 256U, 0U);
    fields.temperature_celsius.assign(256U * 256U, 15.0F);
    fields.ice_surface_kind.assign(256U * 256U, 0U);
    fields.wind_u.assign(256U * 256U, 0.0F);
    fields.wind_v.assign(256U * 256U, 0.0F);
    fields.precipitation_mm_year.assign(256U * 256U, 3'000.0F);
    for (uint32_t y = 0U; y < 256U; ++y)
        for (uint32_t x = 128U; x < 256U; ++x)
            fields.precipitation_mm_year[y * 256U + x] = 500.0F;
    auto dry = std::make_shared<generation::GenerationSnapshot>(*rainy);
    dry->v9->precipitation_mm_year.assign(256U * 256U, 0.0F);
    client::GenerationMapAnimator wet_animator, dry_animator;
    client::GenerationMapAnimationClock const clock{
        .visible_elapsed = std::chrono::milliseconds{4'125}, .formation_duration = std::chrono::seconds{10}};
    auto wet = wet_animator.update(rainy, nullptr, nullptr, nullptr, clock);
    auto clear = dry_animator.update(dry, nullptr, nullptr, nullptr, clock);
    uint32_t weak_region_rain = 0U;
    for (uint32_t y = 16U; y < 240U; ++y)
        for (uint32_t x = 160U; x < 224U; ++x)
        {
            uint32_t const index = animationRenderPixelIndex(x, y);
            weak_region_rain += maximumChannelDistance(wet.image->rgba[index], clear.image->rgba[index]) > 12U;
        }
    EXPECT_GT(weak_region_rain, 40U);
}

TEST(GenerationMapPresentationTest, NativeSeaIceRetainsTheUnderlyingBathymetricContrast)
{
    auto snapshot = makeEngagementSnapshot();
    auto& fields = *snapshot->v9;
    std::fill(fields.ocean_mask.begin(), fields.ocean_mask.end(), 1U);
    std::fill(fields.temperature_celsius.begin(), fields.temperature_celsius.end(), -5.0F);
    std::fill(fields.wind_u.begin(), fields.wind_u.end(), 0.0F);
    std::fill(fields.wind_v.begin(), fields.wind_v.end(), 0.0F);
    std::fill(fields.current_u.begin(), fields.current_u.end(), 0.0F);
    std::fill(fields.current_v.begin(), fields.current_v.end(), 0.0F);
    for (uint32_t y = 0U; y < 256U; ++y)
    {
        fields.ice_surface_kind[y * 256U] = client::GENERATION_MAP_ICE_WATER;
        for (uint32_t x = 0U; x < 256U; ++x)
        {
            fields.source_elevation_m[y * 256U + x] = x < 128U ? -200.0F : -1'000.0F;
        }
    }
    client::GenerationMapAnimator animator;
    auto render = [&](int64_t const milliseconds) {
        return animator.update(snapshot, nullptr, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{milliseconds},
             .formation_duration = std::chrono::seconds{10}});
    };
    auto before = render(9'000);
    auto peak = render(11'250);
    uint32_t const shallow = animationRenderPixelIndex(50U, 100U);
    uint32_t const deep = animationRenderPixelIndex(200U, 100U);
    uint32_t const contrast = maximumChannelDistance(before.image->rgba[shallow], before.image->rgba[deep]);
    ASSERT_GT(contrast, 10U);
    EXPECT_GE(maximumChannelDistance(peak.image->rgba[shallow], peak.image->rgba[deep]), contrast / 2U);
}

TEST(GenerationMapPresentationTest, NativeTectonicDriftMovesACoastVisiblyAndSettlesMonotonically)
{
    auto snapshot = makeEngagementSnapshot();
    auto& fields = *snapshot->v9;
    fields.plate_ids.assign(256U * 256U, 1U);
    fields.plate_directions = {{1U, 1.0F, 0.0F}};
    std::fill(fields.precipitation_mm_year.begin(), fields.precipitation_mm_year.end(), 0.0F);
    std::fill(fields.wind_u.begin(), fields.wind_u.end(), 0.0F);
    std::fill(fields.wind_v.begin(), fields.wind_v.end(), 0.0F);
    std::fill(fields.current_u.begin(), fields.current_u.end(), 0.0F);
    std::fill(fields.current_v.begin(), fields.current_v.end(), 0.0F);
    for (uint32_t y = 0U; y < 256U; ++y)
    {
        for (uint32_t x = 0U; x < 256U; ++x)
        {
            fields.source_elevation_m[y * 256U + x] = x < 128U ? -200.0F : 500.0F;
            fields.ocean_mask[y * 256U + x] = x < 128U ? 1U : 0U;
        }
    }
    client::GenerationMapAnimator animator;
    auto render = [&](int64_t const milliseconds) {
        return animator.update(snapshot, nullptr, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{milliseconds},
             .formation_duration = std::chrono::seconds{10}});
    };
    auto water_end = render(5'249);
    auto start = render(5'250);
    auto middle = render(7'125);
    auto settled = render(8'999);
    EXPECT_EQ(water_end.image->rgba, start.image->rgba);
    auto coast = [](auto const& image) {
        for (uint32_t x = 128U; x < 384U; ++x)
        {
            auto const& pixel = image->rgba[256U * image->width + x];
            if (pixel[0U] > pixel[2U] + 10U)
            {
                return static_cast<double>(x);
            }
        }
        return 0.0;
    };
    EXPECT_LT(coast(start.image), coast(middle.image));
    EXPECT_LT(coast(middle.image), coast(settled.image));
    EXPECT_GT(coast(settled.image) - coast(start.image), 24.0);
}

TEST(GenerationMapPresentationTest, RainFollowsItsSourceRegionWhileTheWorldIsDisplaced)
{
    auto rainy = makeEngagementSnapshot();
    auto& fields = *rainy->v9;
    fields.source_elevation_m.assign(256U * 256U, 500.0F);
    fields.ocean_mask.assign(256U * 256U, 0U);
    fields.plate_ids.assign(256U * 256U, 1U);
    fields.plate_directions = {{1U, 1.0F, 0.0F}};
    std::fill(fields.precipitation_mm_year.begin(), fields.precipitation_mm_year.end(), 0.0F);
    std::fill(fields.wind_u.begin(), fields.wind_u.end(), 0.0F);
    std::fill(fields.wind_v.begin(), fields.wind_v.end(), 0.0F);
    for (uint32_t y = 120U; y <= 144U; ++y)
    {
        for (uint32_t x = 124U; x <= 140U; ++x)
        {
            fields.precipitation_mm_year[y * 256U + x] = 500.0F;
        }
    }
    auto dry = std::make_shared<generation::GenerationSnapshot>(*rainy);
    std::fill(dry->v9->precipitation_mm_year.begin(), dry->v9->precipitation_mm_year.end(), 0.0F);
    client::GenerationMapAnimator rainy_animator;
    client::GenerationMapAnimator dry_animator;
    client::GenerationMapAnimationClock const clock{
        .visible_elapsed = std::chrono::milliseconds{4'125},
        .formation_duration = std::chrono::seconds{10},
    };
    auto rain = rainy_animator.update(rainy, nullptr, nullptr, nullptr, clock);
    auto clear = dry_animator.update(dry, nullptr, nullptr, nullptr, clock);
    uint64_t sum_x = 0U;
    uint32_t changed = 0U;
    for (uint32_t index = 0U; index < rain.image->rgba.size(); ++index)
    {
        if (rain.image->rgba[index] != clear.image->rgba[index])
        {
            sum_x += index % rain.image->width;
            ++changed;
        }
    }
    ASSERT_GT(changed, 0U);
    EXPECT_LT(static_cast<double>(sum_x) / changed / client::GENERATION_MAP_ANIMATION_SCALE, 115.0);
}

TEST(GenerationMapPresentationTest, NativeMovingPlateRetainsLandWhenItCrossesAnotherPlate)
{
    auto snapshot = makeEngagementSnapshot();
    auto& fields = *snapshot->v9;
    fields.plate_directions = {{1U, -1.0F, 0.0F}, {2U, 0.0F, 0.0F}};
    std::fill(fields.precipitation_mm_year.begin(), fields.precipitation_mm_year.end(), 0.0F);
    std::fill(fields.wind_u.begin(), fields.wind_u.end(), 0.0F);
    std::fill(fields.wind_v.begin(), fields.wind_v.end(), 0.0F);
    std::fill(fields.current_u.begin(), fields.current_u.end(), 0.0F);
    std::fill(fields.current_v.begin(), fields.current_v.end(), 0.0F);
    for (uint32_t y = 0U; y < 256U; ++y)
    {
        for (uint32_t x = 0U; x < 256U; ++x)
        {
            size_t const index = static_cast<size_t>(y) * 256U + x;
            bool const land = x >= 96U && x < 128U;
            fields.source_elevation_m[index] = land ? 500.0F : -200.0F;
            fields.ocean_mask[index] = land ? 0U : 1U;
        }
    }
    client::GenerationMapAnimator animator;
    auto moving = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{5'250}, .formation_duration = std::chrono::seconds{10}});
    auto settled = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{8'999}, .formation_duration = std::chrono::seconds{10}});
    uint32_t const crossed = animationRenderPixelIndex(136U, 128U);
    EXPECT_GT(moving.image->rgba[crossed][0U], moving.image->rgba[crossed][2U] + 10U);
    EXPECT_LT(settled.image->rgba[crossed][0U], settled.image->rgba[crossed][2U]);
    auto ice_start = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{9'000}, .formation_duration = std::chrono::seconds{10}});
    EXPECT_EQ(settled.image->rgba, ice_start.image->rgba);
}

TEST(GenerationMapPresentationTest, SettledWaterKeepsTheSamePaletteDuringTectonics)
{
    auto snapshot = makeEngagementSnapshot();
    auto& fields = *snapshot->v9;
    fields.source_elevation_m.assign(256U * 256U, -200.0F);
    fields.ocean_mask.assign(256U * 256U, 1U);
    fields.plate_directions = {{1U, 0.0F, 0.0F}, {2U, 0.0F, 0.0F}};
    std::fill(fields.precipitation_mm_year.begin(), fields.precipitation_mm_year.end(), 0.0F);
    std::fill(fields.wind_u.begin(), fields.wind_u.end(), 0.0F);
    std::fill(fields.wind_v.begin(), fields.wind_v.end(), 0.0F);
    std::fill(fields.current_u.begin(), fields.current_u.end(), 0.0F);
    std::fill(fields.current_v.begin(), fields.current_v.end(), 0.0F);
    client::GenerationMapAnimator animator;
    auto water = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{5'249}, .formation_duration = std::chrono::seconds{10}});
    auto tectonics = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{6'000}, .formation_duration = std::chrono::seconds{10}});
    uint32_t maximum_distance = 0U;
    for (size_t index = 0U; index < water.image->rgba.size(); ++index)
    {
        maximum_distance = std::max(maximum_distance,
            maximumChannelDistance(water.image->rgba[index], tectonics.image->rgba[index]));
    }
    EXPECT_LE(maximum_distance, 2U);
}

TEST(GenerationMapPresentationTest, OpeningRiftJoinsShallowCoastsToADeeperLocalFloor)
{
    auto snapshot = makeEngagementSnapshot();
    auto& fields = *snapshot->v9;
    fields.plate_directions = {{1U, 1.0F, 0.0F}, {2U, -1.0F, 0.0F}};
    std::fill(fields.precipitation_mm_year.begin(), fields.precipitation_mm_year.end(), 0.0F);
    std::fill(fields.wind_u.begin(), fields.wind_u.end(), 0.0F);
    std::fill(fields.wind_v.begin(), fields.wind_v.end(), 0.0F);
    std::fill(fields.current_u.begin(), fields.current_u.end(), 0.0F);
    std::fill(fields.current_v.begin(), fields.current_v.end(), 0.0F);
    for (uint32_t y = 0U; y < 256U; ++y)
    {
        for (uint32_t x = 0U; x < 256U; ++x)
        {
            bool const water = x >= 48U && x < 80U;
            fields.source_elevation_m[y * 256U + x] = water ? -1'000.0F : 500.0F;
            fields.ocean_mask[y * 256U + x] = water ? 1U : 0U;
        }
    }
    client::GenerationMapAnimator animator;
    auto frame = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{5'250}, .formation_duration = std::chrono::seconds{10}});
    uint32_t maximum_edge = 0U;
    std::vector<Rgba8> colors;
    for (uint32_t x = 100U; x < 156U; ++x)
    {
        auto const pixel = frame.image->rgba[animationRenderPixelIndex(x, 128U)];
        auto const next = frame.image->rgba[animationRenderPixelIndex(x + 1U, 128U)];
        maximum_edge = std::max(maximum_edge, maximumChannelDistance(pixel, next));
        if (x >= 108U && x <= 148U && std::find(colors.begin(), colors.end(), pixel) == colors.end())
        {
            colors.push_back(pixel);
        }
    }
    EXPECT_GT(colors.size(), 5U);
    EXPECT_LT(maximum_edge, 100U);
    auto const center = frame.image->rgba[animationRenderPixelIndex(128U, 128U)];
    EXPECT_GT(center[2U], center[0U]);
}

TEST(GenerationMapPresentationTest, MoltenSurfaceStartsHotAndMovesBeforeGeographyEmerges)
{
    auto original = makeEngagementSnapshot();
    auto inverted = std::make_shared<generation::GenerationSnapshot>(*original);
    for (float& height : inverted->v9->source_elevation_m) height = -height;
    client::GenerationMapAnimator original_animator, inverted_animator;
    auto render = [](auto& animator, auto const& snapshot, int64_t elapsed) {
        return animator.update(snapshot, nullptr, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{elapsed}, .formation_duration = std::chrono::seconds{10}});
    };
    auto initial = render(original_animator, original, 0);
    auto inverted_initial = render(inverted_animator, inverted, 0);
    EXPECT_EQ(initial.image->rgba, inverted_initial.image->rgba);
    auto flowing = render(original_animator, original, 750);
    uint32_t changed = 0U, hot_fissures = 0U, dark_rafts = 0U;
    uint32_t minimum_green = 255U, maximum_green = 0U, maximum_edge = 0U;
    for (size_t index = 0U; index < initial.image->rgba.size(); ++index)
    {
        auto const pixel = initial.image->rgba[index];
        changed += maximumChannelDistance(pixel, flowing.image->rgba[index]) > 15U;
        hot_fissures += pixel[0U] > 220U && pixel[1U] > 130U;
        dark_rafts += pixel[0U] < 70U && pixel[1U] < 50U;
        minimum_green = std::min(minimum_green, static_cast<uint32_t>(pixel[1U]));
        maximum_green = std::max(maximum_green, static_cast<uint32_t>(pixel[1U]));
        if (index % initial.image->width != 0U)
            maximum_edge = std::max(maximum_edge, maximumChannelDistance(pixel, initial.image->rgba[index - 1U]));
    }
    EXPECT_GT(maximum_green - minimum_green, 35U);
    EXPECT_LT(maximum_edge, 35U);
    EXPECT_GT(changed, initial.image->rgba.size() / 5U);
    EXPECT_GT(hot_fissures, initial.image->rgba.size() / 100U);
    EXPECT_EQ(dark_rafts, 0U);
    EXPECT_TRUE(std::all_of(initial.image->rgba.begin(), initial.image->rgba.end(), [](auto const& pixel) {
        return pixel[0U] > 200U && pixel[1U] > 80U;
    }));
    auto cooled = render(original_animator, original, 2'999);
    auto water_start = render(original_animator, original, 3'000);
    EXPECT_EQ(cooled.image->rgba, water_start.image->rgba);

    auto waiting_source = std::make_shared<generation::GenerationSnapshot>(*original);
    waiting_source->stage = generation::GenerationStage::V9Climate;
    waiting_source->v9->precipitation_mm_year.clear();
    waiting_source->v9->ice_surface_kind.clear();
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*waiting_source));
    client::GenerationMapAnimator waiting_animator;
    static_cast<void>(render(waiting_animator, waiting_source, 0));
    auto held = render(waiting_animator, waiting_source, 60'000);
    auto circulating = render(waiting_animator, waiting_source, 60'084);
    ASSERT_NE(held.image, nullptr);
    ASSERT_NE(circulating.image, nullptr);
    EXPECT_FLOAT_EQ(held.phase_progress, circulating.phase_progress);
    EXPECT_NE(held.image->rgba, circulating.image->rgba);
}

TEST(GenerationMapPresentationTest, RisingContinentalReliefSolidifiesBeforeLowBasins)
{
    auto snapshot = makeEngagementSnapshot();
    auto& fields = *snapshot->v9;
    fields.plate_ids.assign(256U * 256U, 1U);
    fields.plate_directions = {{1U, 0.0F, 0.0F}};
    fields.ocean_mask.assign(256U * 256U, 0U);
    for (uint32_t y = 0U; y < 256U; ++y)
        for (uint32_t x = 0U; x < 256U; ++x)
            fields.source_elevation_m[y * 256U + x] = static_cast<float>(x) * 4.0F;
    client::GenerationMapAnimator animator;
    auto middle = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{1'500}, .formation_duration = std::chrono::seconds{10}});
    ASSERT_NE(middle.image, nullptr);
    uint64_t low_heat = 0U, high_heat = 0U;
    for (uint32_t y = 32U; y < 224U; ++y)
    {
        low_heat += middle.image->rgba[animationRenderPixelIndex(32U, y)][0U];
        high_heat += middle.image->rgba[animationRenderPixelIndex(224U, y)][0U];
    }
    EXPECT_GT(low_heat, high_heat + 192U * 20U);
}

TEST(GenerationMapPresentationTest, CooledLandIsDarkReliefBeforeWaterRevealsTerrain)
{
    auto snapshot = makeEngagementSnapshot();
    std::fill(snapshot->v9->ocean_mask.begin(), snapshot->v9->ocean_mask.end(), 0U);
    std::fill(snapshot->v9->precipitation_mm_year.begin(), snapshot->v9->precipitation_mm_year.end(), 0.0F);
    for (float& height : snapshot->v9->source_elevation_m) height = std::abs(height) + 100.0F;
    client::GenerationMapAnimator animator;
    auto crust = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{2'999}, .formation_duration = std::chrono::seconds{10}});
    auto terrain = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{5'249}, .formation_duration = std::chrono::seconds{10}});
    uint32_t lightest = 0U, darkest = 255U;
    for (auto const& pixel : crust.image->rgba)
    {
        lightest = std::max(lightest, static_cast<uint32_t>(pixel[0U]));
        darkest = std::min(darkest, static_cast<uint32_t>(pixel[0U]));
    }
    EXPECT_LT(lightest, 110U);
    EXPECT_GT(lightest - darkest, 8U);
    EXPECT_NE(crust.image->rgba, terrain.image->rgba);
}

TEST(GenerationMapPresentationTest, BiomesAppearDuringIceRetreatAndKeepClimateBoundaryContinuous)
{
    auto snapshot = makeEngagementSnapshot();
    auto& fields = *snapshot->v9;
    fields.source_elevation_m.assign(256U * 256U, 500.0F);
    fields.ocean_mask.assign(256U * 256U, 0U);
    fields.ice_surface_kind.assign(256U * 256U, 0U);
    fields.wind_u.assign(256U * 256U, 0.0F);
    fields.wind_v.assign(256U * 256U, 0.0F);
    fields.current_u.assign(256U * 256U, 0.0F);
    fields.current_v.assign(256U * 256U, 0.0F);
    auto final_map = std::make_shared<client::GenerationMapImage>();
    final_map->width = client::GENERATION_MAP_ANIMATION_RENDER_RESOLUTION;
    final_map->height = final_map->width;
    Rgba8 const biome{10U, 240U, 20U, 255U};
    final_map->rgba.assign(final_map->width * final_map->height, biome);
    client::GenerationMapAnimator animator;
    auto render = [&](int64_t elapsed) {
        return animator.update(snapshot, nullptr, final_map, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{elapsed}, .formation_duration = std::chrono::seconds{10}});
    };
    auto before = render(9'000);
    auto retreat = render(12'375);
    auto ice_end = render(13'499);
    auto climate = render(13'500);
    uint32_t const index = animationRenderPixelIndex(128U, 128U);
    EXPECT_LT(maximumChannelDistance(retreat.image->rgba[index], biome),
        maximumChannelDistance(before.image->rgba[index], biome));
    EXPECT_LE(maximumChannelDistance(ice_end.image->rgba[index], climate.image->rgba[index]), 2U);
    EXPECT_EQ(render(15'000).image->rgba, final_map->rgba);
}

TEST(GenerationMapPresentationTest, PacedFormationChangesStagesAndEndsOnExactActualMap)
{
    auto snapshot = makeEngagementSnapshot();
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*snapshot));
    auto final_pixels = client::rasterizeGenerationMap(*snapshot);
    ASSERT_TRUE(final_pixels);
    auto final_image = std::make_shared<client::GenerationMapImage const>(std::move(*final_pixels));
    client::GenerationMapAnimator animator;
    auto render = [&](int64_t milliseconds) {
        return animator.update(snapshot, nullptr, final_image, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{milliseconds}, .ready = true,
             .formation_duration = std::chrono::seconds{10}});
    };
    auto initial = render(0);
    EXPECT_EQ(render(10).image, initial.image);
    EXPECT_NE(render(40).image, initial.image);
    auto early = render(500);
    auto crust = render(2'100);
    auto flooded = render(5'900);
    auto relief = render(8'000);
    ASSERT_NE(early.image, nullptr);
    ASSERT_NE(relief.image, nullptr);
    ASSERT_NE(flooded.image, nullptr);
    auto dry_count = [](auto const& image) {
        return std::count_if(image->rgba.begin(), image->rgba.end(), [](Rgba8 const& pixel) {
            return pixel[0U] > pixel[2U];
        });
    };
    EXPECT_NE(dry_count(crust.image), dry_count(relief.image));
    EXPECT_NE(dry_count(relief.image), dry_count(flooded.image));
    EXPECT_NE(early.image->rgba, relief.image->rgba);
    EXPECT_EQ(render(14'000).phase, client::GenerationMapAnimationPhase::Climate);
    EXPECT_NE(render(14'999).image, final_image);
    auto arrival = render(15'000);
    EXPECT_EQ(arrival.phase, client::GenerationMapAnimationPhase::Arrival);
    EXPECT_NE(arrival.image, final_image);
    EXPECT_EQ(arrival.image->width, final_image->width);
    EXPECT_EQ(arrival.image->height, final_image->height);
    EXPECT_NE(arrival.image->rgba, final_image->rgba);
}

TEST(GenerationMapPresentationTest, NativeWindAndCurrentFieldsEachProduceMovingArrows)
{
    auto snapshot = makeEngagementSnapshot();
    auto without_wind = std::make_shared<generation::GenerationSnapshot>(*snapshot);
    without_wind->v9->wind_u.clear();
    without_wind->v9->wind_v.clear();
    auto without_currents = std::make_shared<generation::GenerationSnapshot>(*snapshot);
    without_currents->v9->current_u.clear();
    without_currents->v9->current_v.clear();
    auto render = [](auto const& source, int64_t milliseconds) {
        client::GenerationMapAnimator animator;
        return animator.update(source, nullptr, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{milliseconds},
             .formation_duration = std::chrono::seconds{10}}).image;
    };
    auto first = render(snapshot, 12'500);
    auto second = render(snapshot, 13'000);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first->rgba, second->rgba);
    EXPECT_NE(first->rgba, render(without_wind, 12'500)->rgba);
    EXPECT_NE(first->rgba, render(without_currents, 12'500)->rgba);
    without_wind->v9->wind_u.assign(1U, 1.0F);
    EXPECT_FALSE(generation::isValidGenerationSnapshot(*without_wind));
}

TEST(GenerationMapPresentationTest, ReadySnapshotKeepsWindAndCurrentAnimationMoving)
{
    auto const snapshot = makeEngagementSnapshot();
    auto final_pixels = client::rasterizeGenerationMap(*snapshot);
    ASSERT_TRUE(final_pixels);
    auto const final_image = std::make_shared<client::GenerationMapImage const>(std::move(*final_pixels));
    auto renderReadyAt = [&](int64_t const milliseconds) {
        return client::animateGenerationMap(
            *snapshot,
            final_image,
            nullptr,
            {
                .visible_elapsed = std::chrono::milliseconds{milliseconds},
                .ready = true,
                .formation_duration = std::chrono::seconds{10},
            }
        );
    };

    auto const first = renderReadyAt(16'000);
    auto const second = renderReadyAt(16'500);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first, final_image);
    EXPECT_NE(first->rgba, second->rgba);
}

TEST(GenerationMapPresentationTest, WindArrowKeepsPathDirectionAtArrivalLoopEndpoint)
{
    auto snapshot = makeEngagementSnapshot();
    auto& fields = *snapshot->v9;
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr uint32_t FIELD_COUNT = RESOLUTION * RESOLUTION;
    fields.wind_u.assign(FIELD_COUNT, 0.0F);
    fields.wind_v.assign(FIELD_COUNT, 0.0F);
    fields.current_u.assign(FIELD_COUNT, 0.0F);
    fields.current_v.assign(FIELD_COUNT, 0.0F);
    fields.precipitation_mm_year.assign(FIELD_COUNT, 0.0F);
    for (uint32_t y = 16U; y < 62U; ++y)
    {
        for (uint32_t x = 16U; x < 62U; ++x)
        {
            if (x == y)
            {
                uint32_t const index = y * RESOLUTION + x;
                fields.wind_u[index] = 1.0F;
                fields.wind_v[index] = 1.0F;
            }
        }
    }
    auto final_pixels = client::rasterizeGenerationMap(*snapshot);
    ASSERT_TRUE(final_pixels);
    auto final_image = std::make_shared<client::GenerationMapImage const>(std::move(*final_pixels));
    client::GenerationMapAnimator animator;
    auto const frame = animator.update(
        snapshot,
        nullptr,
        final_image,
        nullptr,
        {
            .visible_elapsed = std::chrono::milliseconds{15'184},
            .ready = true,
            .formation_duration = std::chrono::seconds{10},
        }
    );
    ASSERT_EQ(frame.phase, client::GenerationMapAnimationPhase::Arrival);
    ASSERT_NE(frame.image, nullptr);

    float const path_position = std::fmod(1.184F * 26.0F, 31.0F);
    float const seed = 0.5F * static_cast<float>(RESOLUTION) / 6.0F;
    float const center = seed + path_position * 1.6F / std::sqrt(2.0F);
    float const pixel_scale = static_cast<float>(frame.image->width) / static_cast<float>(RESOLUTION);
    float const center_x = center * pixel_scale;
    float const center_y = center * pixel_scale;
    bool found_forward_tail = false;
    client::GenerationMapRgba8 const wind_color{62U, 232U, 216U, 255U};
    for (int32_t distance = 6; distance <= 9; ++distance)
    {
        for (int32_t lateral = -2; lateral <= 2; ++lateral)
        {
            int32_t const x = static_cast<int32_t>(std::lround(
                center_x - 0.707'106'78F * static_cast<float>(distance - lateral)
            ));
            int32_t const y = static_cast<int32_t>(std::lround(
                center_y - 0.707'106'78F * static_cast<float>(distance + lateral)
            ));
            if (x >= 0 && y >= 0 && x < static_cast<int32_t>(frame.image->width)
                && y < static_cast<int32_t>(frame.image->height)
                && frame.image->rgba[static_cast<size_t>(y) * frame.image->width + static_cast<uint32_t>(x)]
                    == wind_color)
            {
                found_forward_tail = true;
            }
        }
    }
    EXPECT_TRUE(found_forward_tail);
}

TEST(GenerationMapPresentationTest, NativeInverseDeformationKeepsUniformDryTerrainFilled)
{
    auto snapshot = makeEngagementSnapshot();
    auto& fields = *snapshot->v9;
    fields.source_elevation_m.assign(256U * 256U, 500.0F);
    fields.ocean_mask.assign(256U * 256U, 0U);
    std::fill(fields.precipitation_mm_year.begin(), fields.precipitation_mm_year.end(), 0.0F);
    fields.plate_ids.assign(256U * 256U, 1U);
    fields.plate_directions = {{1U, 1.0F, 0.0F}};
    client::GenerationMapAnimator animator;
    for (int64_t milliseconds : {0, 500, 1'500, 2'500, 3'500})
    {
        auto const frame = animator.update(snapshot, nullptr, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{milliseconds},
             .formation_duration = std::chrono::seconds{10}});
        ASSERT_NE(frame.image, nullptr);
        EXPECT_TRUE(std::all_of(frame.image->rgba.begin(), frame.image->rgba.end(), [](Rgba8 const& pixel) {
            return pixel[0U] > pixel[2U];
        }));
    }
}

TEST(GenerationMapPresentationTest, NativePlateFormationRotatesAnAsymmetricLandmarkBeforeRestoringItsOrientation)
{
    auto snapshot = makeEngagementSnapshot();
    auto& fields = *snapshot->v9;
    for (uint32_t y = 0U; y < 256U; ++y)
    {
        for (uint32_t x = 0U; x < 256U; ++x)
        {
            size_t const index = static_cast<size_t>(y) * 256U + x;
            float const dx = static_cast<float>(x) - 128.0F;
            float const dy = static_cast<float>(y) - 128.0F;
            bool const land = dx * dx / (48.0F * 48.0F) + dy * dy / (12.0F * 12.0F) < 1.0F;
            fields.source_elevation_m[index] = land ? 500.0F : -200.0F;
            fields.ocean_mask[index] = land ? 0U : 1U;
            fields.plate_ids[index] = x >= 64U && x < 192U && y >= 64U && y < 192U ? 1U : 2U;
        }
    }
    auto orientation = [](client::GenerationMapImage const& image) {
        double xx = 0.0;
        double yy = 0.0;
        double xy = 0.0;
        double sum_x = 0.0;
        double sum_y = 0.0;
        double count = 0.0;
        for (uint32_t y = 0U; y < image.height; ++y)
        {
            for (uint32_t x = 0U; x < image.width; ++x)
            {
                auto const pixel = image.rgba[static_cast<size_t>(y) * image.width + x];
                if (pixel[0U] > pixel[2U])
                {
                    double const dx = static_cast<double>(x) - image.width / 2.0;
                    double const dy = static_cast<double>(y) - image.height / 2.0;
                    xx += dx * dx;
                    yy += dy * dy;
                    xy += dx * dy;
                    sum_x += dx;
                    sum_y += dy;
                    count += 1.0;
                }
            }
        }
        xx -= sum_x * sum_x / count;
        yy -= sum_y * sum_y / count;
        xy -= sum_x * sum_y / count;
        return std::atan2(2.0 * xy, xx - yy) * 0.5;
    };
    client::GenerationMapAnimator animator;
    auto early = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{5'500}, .formation_duration = std::chrono::seconds{10}});
    auto restored = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{8'999}, .formation_duration = std::chrono::seconds{10}});
    ASSERT_NE(early.image, nullptr);
    ASSERT_NE(restored.image, nullptr);
    EXPECT_GT(std::abs(orientation(*early.image) - orientation(*restored.image)), 0.05);
    EXPECT_LT(std::abs(orientation(*restored.image)), 0.02);
}

TEST(GenerationMapPresentationTest, V9CoolingKeepsCrustTransitionGradualAcrossOpening)
{
    static constexpr uint32_t RESOLUTION = 256U;
    auto snapshot = makeEngagementSnapshot();
    auto& fields = *snapshot->v9;
    for (uint32_t y = 0U; y < RESOLUTION; ++y)
    {
        for (uint32_t x = 0U; x < RESOLUTION; ++x)
        {
            size_t const index = static_cast<size_t>(y) * RESOLUTION + x;
            fields.source_elevation_m[index] = 1'000.0F * static_cast<float>(x)
                / static_cast<float>(RESOLUTION - 1U);
        }
    }
    fields.plate_ids.assign(static_cast<size_t>(RESOLUTION) * RESOLUTION, 1U);
    fields.plate_directions = {{1U, 0.0F, 0.0F}};
    std::fill(fields.ocean_mask.begin(), fields.ocean_mask.end(), 0U);

    client::GenerationMapAnimator animator;
    auto const molten = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{0}, .formation_duration = std::chrono::seconds{10}});
    auto const cooling_mid = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{1'500}, .formation_duration = std::chrono::seconds{10}});
    auto const cooled = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{2'999}, .formation_duration = std::chrono::seconds{10}});
    ASSERT_NE(molten.image, nullptr);
    ASSERT_NE(cooling_mid.image, nullptr);
    ASSERT_NE(cooled.image, nullptr);

    uint32_t const pixel_index = animationRenderPixelIndex(128U, 0U);
    auto color_distance = [](Rgba8 const& left, Rgba8 const& right) {
        uint32_t distance = 0U;
        for (uint32_t channel = 0U; channel < 3U; ++channel)
        {
            distance += static_cast<uint32_t>(std::abs(
                static_cast<int32_t>(left[channel]) - static_cast<int32_t>(right[channel])
            ));
        }
        return distance;
    };
    uint32_t const full_transition = color_distance(
        molten.image->rgba[pixel_index], cooled.image->rgba[pixel_index]
    );
    EXPECT_GT(full_transition, 20U);
    EXPECT_LT(color_distance(molten.image->rgba[pixel_index], cooling_mid.image->rgba[pixel_index]),
        full_transition * 13U / 20U);
    EXPECT_GT(color_distance(cooling_mid.image->rgba[pixel_index], cooled.image->rgba[pixel_index]),
        full_transition / 5U);
}

TEST(GenerationMapPresentationTest, MoltenOpeningCoolsIntoCompleteCrustAndOceans)
{
    auto snapshot = makeEngagementSnapshot();
    std::fill(snapshot->v9->precipitation_mm_year.begin(), snapshot->v9->precipitation_mm_year.end(), 0.0F);
    client::GenerationMapAnimator animator;
    auto molten = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{0}, .formation_duration = std::chrono::seconds{10}});
    auto ocean_pixels = [](auto const& image) {
        return std::count_if(image->rgba.begin(), image->rgba.end(), [](Rgba8 const& pixel) {
            return pixel[2U] > pixel[0U];
        });
    };
    std::array<size_t, 3U> filling{};
    size_t index = 0U;
    std::array<client::GenerationMapImageHandle, 3U> fill_frames;
    for (int64_t milliseconds : {3'300, 4'000, 4'800})
    {
        auto frame = animator.update(snapshot, nullptr, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{milliseconds}, .formation_duration = std::chrono::seconds{10}});
        fill_frames[index] = frame.image;
        filling[index++] = static_cast<size_t>(ocean_pixels(frame.image));
    }
    EXPECT_EQ(filling[0U], 0U);
    EXPECT_LT(filling[0U], filling[1U]);
    EXPECT_LE(filling[1U], filling[2U]);
    ASSERT_TRUE(fill_frames[1U]); ASSERT_TRUE(fill_frames[2U]);
    EXPECT_NE(fill_frames[1U]->rgba, fill_frames[2U]->rgba);
    auto cooled = animator.update(snapshot, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{5'249}, .formation_duration = std::chrono::seconds{10}});
    ASSERT_NE(molten.image, nullptr);
    ASSERT_NE(cooled.image, nullptr);
    EXPECT_EQ(molten.phase, client::GenerationMapAnimationPhase::Creation);
    auto hot_pixels = [](auto const& image) {
        return std::count_if(image->rgba.begin(), image->rgba.end(), [](Rgba8 const& pixel) {
            return pixel[0U] > pixel[1U] * 1.4F && pixel[0U] > pixel[2U] * 2.0F;
        });
    };
    EXPECT_GT(hot_pixels(molten.image), molten.image->rgba.size() / 2U);
    EXPECT_LT(hot_pixels(cooled.image), cooled.image->rgba.size() / 10U);
    EXPECT_TRUE(std::any_of(cooled.image->rgba.begin(), cooled.image->rgba.end(), [](Rgba8 const& pixel) {
        return pixel[2U] > pixel[0U];
    }));
    EXPECT_TRUE(std::all_of(cooled.image->rgba.begin(), cooled.image->rgba.end(), [](Rgba8 const& pixel) {
        return pixel[3U] == 255U;
    }));
}

TEST(GenerationMapPresentationTest, DISABLED_ActualV21CpuRastersForCoherenceEvidence)
{
    char const* const directory = std::getenv("MC_GENERATION_MAP_EVIDENCE_DIR");
    ASSERT_NE(directory, nullptr);
    generation::GenerationConfiguration configuration;
    configuration.model_tag = generation::TerrainModelTag::V21;
    std::optional<generation::GenerationStage> last_stage;
    auto atlas = generation::prepareAtlas(configuration, {}, [&](auto const& progress) {
        if (last_stage != progress.stage)
        {
            std::cout << "[cpu-atlas] stage=" << static_cast<unsigned>(progress.stage) << std::endl;
            last_stage = progress.stage;
        }
    });
    ASSERT_NE(atlas, nullptr);
    uint64_t fingerprint = 14'695'981'039'346'656'037ULL;
    for (auto const& field : generation::v21RegisteredFields(*atlas))
    {
        std::visit([&](auto values) {
            for (auto const value : std::as_bytes(values))
            {
                fingerprint = (fingerprint ^ static_cast<uint8_t>(value)) * 1'099'511'628'211ULL;
            }
        }, field.values);
    }
    std::ofstream metadata(std::filesystem::path(directory) / "source-and-cost.csv");
    metadata << "source_fnv64," << fingerprint << '\n';
    metadata << "configuration," << generation::configurationIdentity(configuration) << '\n';
    metadata << "elapsed_ms,update_us,cpu_us\n";
    shared::TerrainWorld world{atlas};
    auto lod = shared::buildTerrainLod(world, shared::TerrainLodLevel::L0);
    ASSERT_NE(lod, nullptr);
    auto final_map = client::rasterizeS2WorldMap(world, *lod, {});
    ASSERT_TRUE(final_map);
    auto final_image = std::make_shared<client::GenerationMapImage const>(client::GenerationMapImage{
        .width = final_map->width, .height = final_map->height, .rgba = std::move(final_map->rgba)});
    EXPECT_TRUE(writePpmIfRequested("final-map-source.ppm", *final_image));
    client::GenerationMapAnimator animator;
    std::vector<int64_t> timestamps{0, 750, 1'500, 2'250, 2'999, 3'000, 4'125, 5'249, 5'250, 6'500, 7'125,
        8'999, 9'000, 10'125, 11'250, 12'375, 13'499, 13'500, 14'250, 14'999, 15'000, 16'000};
    if (std::getenv("MC_GENERATION_MAP_MOLTEN_TICKS") != nullptr)
    {
        for (int64_t elapsed = 42; elapsed < 3'000; elapsed += 42) timestamps.push_back(elapsed);
        std::sort(timestamps.begin(), timestamps.end());
        timestamps.erase(std::unique(timestamps.begin(), timestamps.end()), timestamps.end());
    }
    for (int64_t const elapsed : timestamps)
    {
        auto const started = std::chrono::steady_clock::now();
        auto const cpu_started = std::clock();
        auto frame = animator.update(nullptr, atlas, elapsed >= 9'000 ? final_image : nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{elapsed}, .formation_duration = std::chrono::seconds{10}});
        auto const cpu_finished = std::clock();
        auto const microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        ASSERT_NE(frame.image, nullptr);
        if (elapsed >= 15'000)
        {
            // Arrival deliberately adds live wind/current strokes to the exact
            // final terrain image. Preserve both rasters for separate comparison.
            size_t changed = 0U;
            for (size_t index = 0U; index < final_image->rgba.size(); ++index)
                changed += frame.image->rgba[index] != final_image->rgba[index];
            std::cout << "[cpu-arrival-weather] elapsed_ms=" << elapsed << " changed_pixels=" << changed << std::endl;
        }
        EXPECT_TRUE(writePpmIfRequested("tectonics-" + std::to_string(elapsed) + ".ppm", *frame.image));
        auto const cpu_microseconds = static_cast<int64_t>(static_cast<double>(cpu_finished - cpu_started)
            * 1'000'000.0 / CLOCKS_PER_SEC);
        metadata << elapsed << ',' << microseconds << ',' << cpu_microseconds << '\n';
        std::cout << "[cpu-raster] elapsed_ms=" << elapsed << " update_us=" << microseconds << std::endl;
    }
    EXPECT_TRUE(metadata.good());
}

} // namespace
