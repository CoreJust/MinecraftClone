#include <client/GenerationMapPresentation.hpp>

#include <client/S2MapPresentation.hpp>

#include <generation/Generation.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
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

TEST(GenerationMapPresentationTest, AnimationGrowsIceMonotonicallyTowardTheSharedMask)
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

    client::GenerationMapAnimationClock const early_clock{
        .visible_elapsed = std::chrono::milliseconds{ 15'100 },
    };
    client::GenerationMapAnimationClock const late_clock{
        .visible_elapsed = std::chrono::milliseconds{ 20'000 },
    };
    std::shared_ptr<client::GenerationMapImage const> const early = client::animateGenerationMap(
        snapshot,
        nullptr,
        mask_handle,
        early_clock
    );
    std::shared_ptr<client::GenerationMapImage const> const late = client::animateGenerationMap(
        snapshot,
        nullptr,
        mask_handle,
        late_clock
    );

    ASSERT_NE(early, nullptr);
    ASSERT_NE(late, nullptr);
    uint32_t const colder_pixel = animationRenderPixelIndex(0U, 0U);
    uint32_t const warmer_pixel = animationRenderPixelIndex(WARMER_SOURCE_INDEX, 0U);
    EXPECT_GT(early->rgba[colder_pixel][0U], early->rgba[warmer_pixel][0U]);
    EXPECT_GT(late->rgba[warmer_pixel][0U], early->rgba[warmer_pixel][0U]);
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
            client::GenerationMapAnimationClock{ .visible_elapsed = visible_elapsed }
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
    client::GenerationMapAnimationFrame const at_fractional_drift = renderAt(std::chrono::milliseconds{ 750 });
    client::GenerationMapAnimationFrame const after_fractional_drift = renderAt(std::chrono::milliseconds{ 1'000 });
    client::GenerationMapAnimationFrame const before_relief = renderAt(std::chrono::milliseconds{ 4'999 });
    client::GenerationMapAnimationFrame const relief_start = renderAt(std::chrono::milliseconds{ 5'000 });
    client::GenerationMapAnimationFrame const relief_after_drift = renderAt(std::chrono::milliseconds{ 5'750 });

    ASSERT_NE(at_start.image, nullptr);
    ASSERT_NE(at_fractional_drift.image, nullptr);
    ASSERT_NE(after_fractional_drift.image, nullptr);
    ASSERT_NE(before_relief.image, nullptr);
    ASSERT_NE(relief_start.image, nullptr);
    ASSERT_NE(relief_after_drift.image, nullptr);
    EXPECT_EQ(at_start.image->width, client::GENERATION_MAP_ANIMATION_RENDER_RESOLUTION);
    EXPECT_EQ(at_start.image->height, client::GENERATION_MAP_ANIMATION_RENDER_RESOLUTION);
    EXPECT_EQ(at_start.phase, client::GenerationMapAnimationPhase::Tectonics);
    EXPECT_EQ(before_relief.phase, client::GenerationMapAnimationPhase::Tectonics);
    EXPECT_EQ(relief_start.phase, client::GenerationMapAnimationPhase::Relief);

    uint32_t const center_edge = NATIVE_EDGE_X * client::GENERATION_MAP_ANIMATION_SCALE
        - 6U * client::GENERATION_MAP_ANIMATION_SCALE;
    uint32_t const edge_first_x = center_edge - EDGE_WINDOW_RADIUS;
    uint32_t const edge_last_x = center_edge + EDGE_WINDOW_RADIUS;
    double const start_edge = verticalEdgePosition(*at_start.image, EDGE_ROW, edge_first_x, edge_last_x);
    double const fractional_edge = verticalEdgePosition(
        *at_fractional_drift.image,
        EDGE_ROW,
        edge_first_x,
        edge_last_x
    );
    double const fractional_shift = fractional_edge - start_edge;
    EXPECT_GE(fractional_shift, 0.0);
    EXPECT_LE(fractional_shift, 1.0);
    EXPECT_GT(verticalEdgePosition(*after_fractional_drift.image, EDGE_ROW, edge_first_x, edge_last_x) - start_edge, 0.5);

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
    EXPECT_TRUE(writePpmIfRequested("tectonics-0750.ppm", *at_fractional_drift.image));
    EXPECT_TRUE(writePpmIfRequested("tectonics-1000.ppm", *after_fractional_drift.image));
    EXPECT_TRUE(writePpmIfRequested("tectonics-4999.ppm", *before_relief.image));
    EXPECT_TRUE(writePpmIfRequested("relief-5000.ppm", *relief_start.image));
    EXPECT_TRUE(writePpmIfRequested("relief-5750.ppm", *relief_after_drift.image));
    EXPECT_TRUE(writeFrameTimingsIfRequested(elapsed_microseconds, phase_indices));
}

TEST(GenerationMapPresentationTest, TectonicDriftPreservesWaterInteriorColorAndShadingSemantics)
{
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr uint32_t FIELD_COUNT = RESOLUTION * RESOLUTION;
    // Flooded cell beside the translated relief: only the dry control receives hillshade.
    static constexpr uint32_t SAMPLE_X = 121U;
    static constexpr uint32_t SAMPLE_Y = 100U;

    auto water_atlas = std::make_shared<generation::GlobalAtlas>(*makeDriftingMapAtlas());
    water_atlas->sea_level = 20.0F;
    water_atlas->water_level.assign(FIELD_COUNT, 20.0F);
    water_atlas->ocean_mask.assign(FIELD_COUNT, 1U);
    water_atlas->lake_mask.assign(FIELD_COUNT, 0U);
    water_atlas->river_mask.assign(FIELD_COUNT, 0U);
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

    client::GenerationMapAnimator water_animator;
    client::GenerationMapAnimator dry_control_animator;
    client::GenerationMapAnimationClock const clock{
        .visible_elapsed = std::chrono::milliseconds{ 750 },
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
    EXPECT_EQ(water_pixel, (Rgba8{ 43U, 86U, 106U, 255U }));
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
        client::GenerationMapAnimationClock{ .visible_elapsed = std::chrono::milliseconds{ 0 } }
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
        client::GenerationMapAnimationClock{ .visible_elapsed = std::chrono::milliseconds{ 0 } }
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
        client::GenerationMapAnimationPhase::Relief,
        client::GenerationMapAnimationPhase::Water,
        client::GenerationMapAnimationPhase::Ice,
        client::GenerationMapAnimationPhase::Climate,
        client::GenerationMapAnimationPhase::Arrival,
    }};

    for (uint32_t index = 0U; index < static_cast<uint32_t>(PHASES.size()); ++index) {
        client::GenerationMapAnimationClock const clock{
            .visible_elapsed = std::chrono::seconds{ index * 5U },
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
    };
    client::GenerationMapAnimationFrame const ready_early = animator.update(
        snapshot, nullptr, final_image, nullptr, ready_early_clock);
    EXPECT_EQ(ready_early.phase, client::GenerationMapAnimationPhase::Water);
    EXPECT_FLOAT_EQ(ready_early.phase_progress, 0.2F);
    EXPECT_NE(ready_early.image, final_image);

    client::GenerationMapAnimationClock const arrival_clock{
        .visible_elapsed = std::chrono::seconds{ 30U },
        .ready = true,
    };
    client::GenerationMapAnimationFrame const arrival = animator.update(
        snapshot, nullptr, final_image, nullptr, arrival_clock);
    EXPECT_EQ(arrival.phase, client::GenerationMapAnimationPhase::Arrival);
    EXPECT_FLOAT_EQ(arrival.phase_progress, 1.0F);
    EXPECT_EQ(arrival.image, final_image);
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
    EXPECT_EQ(initial.phase, client::GenerationMapAnimationPhase::Tectonics);
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
        {.visible_elapsed = std::chrono::milliseconds{750}}));
    static_cast<void>(animator.update(climate, nullptr, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{5'834}}));
    auto render = [&](int64_t const elapsed) {
        return animator.update(water, nullptr, final_image, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{elapsed}, .ready = true});
    };
    auto filling = render(31'000);
    ASSERT_EQ(filling.phase, client::GenerationMapAnimationPhase::Water);
    EXPECT_NE(filling.image, final_image);
    auto ice_start = render(32'000);
    ASSERT_EQ(ice_start.phase, client::GenerationMapAnimationPhase::Ice);
    auto const canonical_base = client::s2NativeTerrainColor(500.0F, {500.0F, 500.0F}, {500.0F, 500.0F}, false, 0.0F, 0U);
    Rgba8 expected_base{0U, 0U, 0U, 255U};
    for (uint32_t channel = 0U; channel < 3U; ++channel)
    {
        expected_base[channel] = static_cast<uint8_t>(std::lround(canonical_base[channel]));
    }
    EXPECT_TRUE(std::all_of(ice_start.image->rgba.begin(), ice_start.image->rgba.end(),
        [&](Rgba8 const& pixel) { return pixel == expected_base; }));
    auto ice_early = render(32'250);
    auto ice_late = render(32'750);
    auto frozen_count = [](auto const& image) {
        return std::count_if(image->rgba.begin(), image->rgba.end(), [](Rgba8 const& pixel) {
            return pixel[1U] >= 235U && pixel[2U] >= 220U;
        });
    };
    EXPECT_GT(frozen_count(ice_late.image), frozen_count(ice_early.image));
    auto climate_start = render(33'000);
    EXPECT_EQ(climate_start.phase, client::GenerationMapAnimationPhase::Climate);
    auto weather = render(33'500);
    EXPECT_EQ(weather.phase, client::GenerationMapAnimationPhase::Climate);
    EXPECT_NE(weather.image->rgba, climate_start.image->rgba);
    EXPECT_GE(frozen_count(weather.image), frozen_count(ice_late.image));
    auto arrival = render(34'000);
    EXPECT_EQ(arrival.phase, client::GenerationMapAnimationPhase::Arrival);
    EXPECT_EQ(arrival.image, final_image);
}

TEST(GenerationMapPresentationTest, MovingNearestSamplesKeepSourceColorsWithoutPhaseOnlyRgbBlur)
{
    auto atlas = std::make_shared<generation::GlobalAtlas>(*makeDriftingMapAtlas());
    uint32_t const count = atlas->configuration.resolution * atlas->configuration.resolution;
    atlas->water_level.assign(count, 0.0F);
    atlas->ocean_mask.assign(count, 1U);
    atlas->lake_mask.assign(count, 0U);
    atlas->river_mask.assign(count, 0U);
    std::array<Rgba8, 2U> const source_colors{{
        {35U, 72U, 94U, 255U}, {229U, 231U, 224U, 255U},
    }};
    client::GenerationMapAnimator animator;
    for (int64_t const elapsed : {0, 750, 1'500, 5'750})
    {
        auto const frame = animator.update(nullptr, atlas, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{elapsed}});
        ASSERT_NE(frame.image, nullptr);
        EXPECT_TRUE(std::all_of(frame.image->rgba.begin(), frame.image->rgba.end(), [&](Rgba8 const& pixel) {
            return std::find(source_colors.begin(), source_colors.end(), pixel) != source_colors.end();
        }));
    }
}

} // namespace
