#include "Numerics.hpp"

#include <generation/Generation.hpp>
#include <generation/GenerationStages.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace
{

generation::GenerationConfiguration configurationFor(uint32_t resolution)
{
    generation::GenerationConfiguration configuration;
    configuration.resolution = resolution;
    return configuration;
}

generation::detail::HydrologyFields build(
    uint32_t resolution,
    std::vector<float> const& elevation,
    std::span<float const> precipitation = {},
    std::span<float const> temperature = {},
    uint32_t erosion_steps = 0U
)
{
    return generation::detail::buildHydrology(
        configurationFor(resolution),
        elevation,
        precipitation,
        temperature,
        generation::detail::HydrologyParameters{
            .erosion_steps = erosion_steps,
            .sea_level = 0.0F,
            .stage = generation::GenerationStage::InitialDrainage,
        },
        {},
        {}
    );
}

template <typename T>
void expectFixtureSamples(std::string const& name, std::span<T const> values)
{
    std::vector<T> samples;
    samples.reserve(16U * 16U);
    constexpr uint32_t STRIDE = 16U;
    for (uint32_t y = 0U; y < 256U; y += STRIDE) {
        for (uint32_t x = 0U; x < 256U; x += STRIDE) {
            samples.push_back(values[static_cast<size_t>(y) * 256U + x]);
        }
    }
    std::ifstream input(std::string(MC_TERRAIN_REFERENCE_FIXTURE_DIR)
        + "/seed-42/" + name + ".raw", std::ios::binary);
    ASSERT_TRUE(input.good()) << "missing fixed Terrain-v2 fixture " << name;
    std::vector<char> expected((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    ASSERT_EQ(expected.size(), samples.size() * sizeof(T)) << name;
    EXPECT_EQ(std::memcmp(expected.data(), samples.data(), expected.size()), 0) << name;
}

template <typename T>
void expectFixtureSamples(std::string const& name, std::vector<T> const& values)
{
    expectFixtureSamples(name, std::span<T const>(values));
}

generation::detail::CoastalFields seed42Coast(generation::GenerationConfiguration const& configuration)
{
    using namespace generation::detail;
    TectonicsFields tectonics = generateTectonics(configuration, {}, {});
    float const sea_shift = medianFloat32(tectonics.elevation);
    for (float& value : tectonics.elevation) value -= sea_shift;
    FloatField activity(tectonics.elevation.size());
    for (size_t index = 0U; index < activity.size(); ++index) {
        float const relative_motion = tectonics.uplift[index] - tectonics.subduction[index]
            + std::abs(tectonics.divergence[index]);
        activity[index] = std::clamp(relative_motion / 2.5F, 0.0F, 1.0F);
    }
    return elaborateCoasts(configuration, tectonics.elevation, activity, {}, {});
}

void expectHydrologyFixtures(
    generation::detail::HydrologyFields const& fields,
    std::string const& prefix
)
{
    expectFixtureSamples(prefix + "_elevation", fields.elevation);
    expectFixtureSamples(prefix + "_filled", fields.filled);
    expectFixtureSamples(prefix + "_water_level", fields.water_level);
    expectFixtureSamples(prefix + "_flow_accumulation", fields.flow_accumulation);
    expectFixtureSamples(prefix + "_downstream", fields.downstream);
    expectFixtureSamples(prefix + "_ocean_mask", fields.ocean_mask);
    expectFixtureSamples(prefix + "_lake_mask", fields.lake_mask);
    expectFixtureSamples(prefix + "_river_mask", fields.river_mask);
}

TEST(HydrologyTests, AllLandDrainsToTheGlobalMinimumClosedSink)
{
    constexpr uint32_t RESOLUTION = 7U;
    std::vector<float> elevation(static_cast<std::vector<float>::size_type>(RESOLUTION) * RESOLUTION, 2.0F);
    constexpr int32_t SINK = 2 * static_cast<int32_t>(RESOLUTION) + 3;
    elevation[static_cast<size_t>(SINK)] = 1.0F;

    generation::detail::HydrologyFields const result = build(RESOLUTION, elevation);

    ASSERT_EQ(result.ocean_mask.size(), elevation.size());
    EXPECT_EQ(result.ocean_mask, std::vector<uint8_t>(
        static_cast<std::vector<uint8_t>::size_type>(RESOLUTION) * RESOLUTION,
        0U
    ));
    ASSERT_EQ(result.downstream.size(), elevation.size());
    EXPECT_EQ(result.downstream[static_cast<size_t>(SINK)], -1);
    EXPECT_EQ(std::count(result.downstream.begin(), result.downstream.end(), -1), 1);
    EXPECT_TRUE(std::all_of(result.outlet_id.begin(), result.outlet_id.end(),
                            [](int32_t outlet) { return outlet == SINK; }));
    EXPECT_FLOAT_EQ(result.flow_accumulation[static_cast<size_t>(SINK)], 49.0F);
}

TEST(HydrologyTests, PeriodicLargestSeaKeepsItsSeamAndDepressionsAsLakes)
{
    constexpr uint32_t RESOLUTION = 9U;
    std::vector<float> elevation(
        static_cast<std::vector<float>::size_type>(RESOLUTION) * RESOLUTION,
        3.0F
    );
    elevation[4U] = -1.0F;
    elevation[static_cast<std::vector<float>::size_type>(3U) * RESOLUTION] = -1.0F;
    elevation[static_cast<std::vector<float>::size_type>(3U) * RESOLUTION + RESOLUTION - 1U] = -1.0F;
    elevation[static_cast<std::vector<float>::size_type>(3U) * RESOLUTION + 1U] = 0.5F;
    elevation[static_cast<std::vector<float>::size_type>(4U) * RESOLUTION + 4U] = 0.5F;
    elevation[static_cast<std::vector<float>::size_type>(4U) * RESOLUTION + 5U] = 0.7F;

    generation::detail::HydrologyFields const result = build(RESOLUTION, elevation);

    EXPECT_EQ(result.sea_components, 2U);
    EXPECT_EQ(result.ocean_mask[
        static_cast<std::vector<uint8_t>::size_type>(3U) * RESOLUTION
    ], 1U);
    EXPECT_EQ(result.ocean_mask[
        static_cast<std::vector<uint8_t>::size_type>(3U) * RESOLUTION + RESOLUTION - 1U
    ], 1U);
    EXPECT_EQ(result.downstream[
        static_cast<std::vector<int32_t>::size_type>(3U) * RESOLUTION + 1U
    ],
              3 * static_cast<int32_t>(RESOLUTION));
    EXPECT_EQ(result.ocean_mask[4U], 0U);
    EXPECT_EQ(result.lake_mask[4U], 1U);
    EXPECT_EQ(result.lake_mask[static_cast<std::vector<uint8_t>::size_type>(4U) * RESOLUTION + 4U], 1U);
    EXPECT_EQ(result.lake_mask[static_cast<std::vector<uint8_t>::size_type>(4U) * RESOLUTION + 5U], 1U);
    EXPECT_EQ(result.lake_id[static_cast<std::vector<int32_t>::size_type>(4U) * RESOLUTION + 4U],
              result.lake_id[static_cast<std::vector<int32_t>::size_type>(4U) * RESOLUTION + 5U]);
    EXPECT_GT(result.lake_id[static_cast<std::vector<int32_t>::size_type>(4U) * RESOLUTION + 4U], 0);
    EXPECT_FLOAT_EQ(result.elevation[static_cast<std::vector<float>::size_type>(4U) * RESOLUTION + 4U], 0.5F);
    EXPECT_FLOAT_EQ(result.filled[static_cast<std::vector<float>::size_type>(4U) * RESOLUTION + 4U], 3.0F);
    EXPECT_FLOAT_EQ(result.water_level[static_cast<std::vector<float>::size_type>(4U) * RESOLUTION + 5U], 3.0F);
}

TEST(HydrologyTests, RainfallAddsItsRunoffToTheOceanOutlet)
{
    constexpr uint32_t RESOLUTION = 7U;
    std::vector<float> elevation(
        static_cast<std::vector<float>::size_type>(RESOLUTION) * RESOLUTION,
        2.0F
    );
    elevation[0] = -1.0F;
    std::vector<float> dry(
        static_cast<std::vector<float>::size_type>(RESOLUTION) * RESOLUTION,
        0.0F
    );
    std::vector<float> rain = dry;
    rain[static_cast<std::vector<float>::size_type>(3U) * RESOLUTION + 3U] = 5.0F;

    generation::detail::HydrologyFields const dry_result = build(
        RESOLUTION, elevation, dry);
    generation::detail::HydrologyFields const rain_result = build(
        RESOLUTION, elevation, rain);

    EXPECT_NEAR(rain_result.flow_accumulation[0] - dry_result.flow_accumulation[0],
                5.0F,
                1.0e-5F);
}

TEST(HydrologyTests, Seed42InitialAndClimateInformedDrainageMatchRetainedStageBytes)
{
    generation::GenerationConfiguration configuration;
    configuration.resolution = 256U;
    generation::detail::CoastalFields const coast = seed42Coast(configuration);
    generation::detail::HydrologyFields const initial = generation::detail::buildHydrology(
        configuration, coast.elevation, {}, {},
        generation::detail::HydrologyParameters{
            .erosion_steps = 0U,
            .sea_level = 0.0F,
            .stage = generation::GenerationStage::InitialDrainage,
        },
        {}, {});
    expectHydrologyFixtures(initial, "stage__initial_drainage");

    generation::detail::ClimateFields const provisional = generation::detail::simulateClimate(
        configuration, coast.elevation,
        generation::detail::ClimateParameters{.iterations = 24U, .sea_level = 0.0F},
        initial.ocean_mask, initial.lake_mask,
        generation::GenerationStage::ProvisionalClimate, {}, {});
    generation::detail::HydrologyFields const erosion = generation::detail::buildHydrology(
        configuration, coast.elevation, provisional.precipitation, provisional.temperature,
        generation::detail::HydrologyParameters{
            .erosion_steps = 3U,
            .sea_level = 0.0F,
            .stage = generation::GenerationStage::TerrainErosion,
        },
        {}, {});
    expectHydrologyFixtures(erosion, "stage__terrain_erosion");
}

} // namespace
