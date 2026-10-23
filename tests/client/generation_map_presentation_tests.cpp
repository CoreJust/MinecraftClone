#include <client/GenerationMapPresentation.hpp>

#include <generation/Generation.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
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

    std::optional<client::GenerationMapImage> image = client::rasterizeGenerationMap(snapshot);

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

TEST(GenerationMapPresentationTest, LandUsesInclusiveSnowBoundaryAndOceanUsesStrictIceBoundary)
{
    static constexpr uint32_t JUST_WARM = 0U;
    static constexpr uint32_t AT_BOUNDARY = 1U;
    static constexpr uint32_t JUST_COLD = 2U;
    static constexpr uint32_t OCEAN_JUST_WARM = 3U;
    static constexpr uint32_t OCEAN_AT_BOUNDARY = 4U;
    static constexpr uint32_t OCEAN_JUST_COLD = 5U;

    generation::GenerationSnapshot snapshot = makeSnapshot(generation::GenerationStage::FinalDrainage);
    addHydrology(snapshot);
    addClimate(snapshot, 5.0F, 0.5F);
    snapshot.temperature[JUST_WARM] = std::nextafter(-2.0F, 0.0F);
    snapshot.temperature[AT_BOUNDARY] = -2.0F;
    snapshot.temperature[JUST_COLD] = std::nextafter(-2.0F, -3.0F);
    snapshot.temperature[OCEAN_JUST_WARM] = std::nextafter(-2.0F, 0.0F);
    snapshot.temperature[OCEAN_AT_BOUNDARY] = -2.0F;
    snapshot.temperature[OCEAN_JUST_COLD] = std::nextafter(-2.0F, -3.0F);
    snapshot.ocean_mask[OCEAN_JUST_WARM] = 1U;
    snapshot.ocean_mask[OCEAN_AT_BOUNDARY] = 1U;
    snapshot.ocean_mask[OCEAN_JUST_COLD] = 1U;
    snapshot.elevation[OCEAN_JUST_WARM] = -0.5F;
    snapshot.elevation[OCEAN_AT_BOUNDARY] = -0.5F;
    snapshot.elevation[OCEAN_JUST_COLD] = -0.5F;

    std::optional<client::GenerationMapImage> image = client::rasterizeGenerationMap(snapshot);
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

} // namespace
