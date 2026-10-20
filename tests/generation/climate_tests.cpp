#include <generation/GenerationStages.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace
{

generation::GenerationConfiguration configurationFor(uint32_t resolution, uint64_t seed = 42U)
{
    generation::GenerationConfiguration configuration;
    configuration.resolution = resolution;
    configuration.seed = seed;
    return configuration;
}

generation::detail::ClimateFields simulate(
    generation::GenerationConfiguration const& configuration,
    std::vector<float> const& elevation,
    uint32_t iterations,
    std::span<uint8_t const> ocean = {},
    std::span<uint8_t const> lakes = {}
)
{
    return generation::detail::simulateClimate(
        configuration,
        elevation,
        iterations,
        0.0F,
        ocean,
        lakes,
        generation::GenerationStage::ProvisionalClimate,
        {},
        {}
    );
}

std::vector<float> ridgeTerrain(uint32_t resolution)
{
    std::vector<float> elevation(resolution * resolution);
    for (uint32_t y = 0U; y < resolution; ++y)
    {
        for (uint32_t x = 0U; x < resolution; ++x)
        {
            double const half = static_cast<double>(resolution) * 0.5;
            double const dx = std::abs(static_cast<double>(x) - half);
            double const periodic_x = std::min(dx, static_cast<double>(resolution) - dx);
            double const normalized = periodic_x / (static_cast<double>(resolution) * 0.035);
            double const ridge = 3.5 * std::exp(-0.5 * normalized * normalized);
            double const land = (
                static_cast<double>(x) >= resolution * 0.2
                    && static_cast<double>(x) < resolution * 0.8
            )
                ? 0.2
                : -2.0;
            elevation[static_cast<size_t>(y) * resolution + x] =
                static_cast<float>(land + ridge);
        }
    }
    return elevation;
}

void expectFixtureSamples(std::string const& name, std::span<float const> values)
{
    std::vector<float> samples;
    samples.reserve(16U * 16U);
    for (uint32_t y = 0U; y < 256U; y += 16U) {
        for (uint32_t x = 0U; x < 256U; x += 16U) {
            samples.push_back(values[static_cast<size_t>(y) * 256U + x]);
        }
    }
    std::ifstream input(std::string(MC_TERRAIN_REFERENCE_FIXTURE_DIR)
        + "/seed-42/" + name + ".raw", std::ios::binary);
    ASSERT_TRUE(input.good()) << "missing fixed Terrain-v2 fixture " << name;
    std::vector<char> expected((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    ASSERT_EQ(expected.size(), samples.size() * sizeof(float)) << name;
    EXPECT_EQ(std::memcmp(expected.data(), samples.data(), expected.size()), 0) << name;
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

TEST(ClimateTests, ToroidalInsolationAppliesLapseAndProducesRepeatableFields)
{
    constexpr uint32_t RESOLUTION = 16U;
    generation::GenerationConfiguration const configuration = configurationFor(RESOLUTION);
    std::vector<float> elevation(RESOLUTION * RESOLUTION, 1.0F);

    generation::detail::ClimateFields const first = simulate(configuration, elevation, 24U);
    generation::detail::ClimateFields const second = simulate(configuration, elevation, 24U);
    std::vector<float> higher_elevation = elevation;
    std::fill(higher_elevation.begin(), higher_elevation.end(), 2.0F);
    generation::detail::ClimateFields const higher =
        simulate(configuration, higher_elevation, 24U);

    ASSERT_EQ(first.temperature.size(), elevation.size());
    EXPECT_FLOAT_EQ(first.temperature[0], -24.5F);
    EXPECT_FLOAT_EQ(first.temperature[8U * RESOLUTION], 23.5F);
    for (size_t index = 0U; index < elevation.size(); ++index)
    {
        EXPECT_FLOAT_EQ(first.temperature[index] - higher.temperature[index], 6.5F);
        EXPECT_FLOAT_EQ(first.temperature[index], second.temperature[index]);
        EXPECT_FLOAT_EQ(first.precipitation[index], second.precipitation[index]);
        EXPECT_FLOAT_EQ(first.moisture[index], second.moisture[index]);
        EXPECT_FLOAT_EQ(first.wind_u[index], second.wind_u[index]);
        EXPECT_FLOAT_EQ(first.wind_v[index], second.wind_v[index]);
        EXPECT_FLOAT_EQ(first.current_u[index], 0.0F);
        EXPECT_FLOAT_EQ(first.current_v[index], 0.0F);
    }
}

TEST(ClimateTests, ReversingWindMovesRidgeRainfallToTheOppositeSlope)
{
    constexpr uint32_t RESOLUTION = 96U;
    generation::GenerationConfiguration east_configuration = configurationFor(RESOLUTION, 7U);
    generation::GenerationConfiguration west_configuration = east_configuration;
    west_configuration.wind_sign = -1;
    std::vector<float> const elevation = ridgeTerrain(RESOLUTION);

    generation::detail::ClimateFields const east =
        simulate(east_configuration, elevation, 64U);
    generation::detail::ClimateFields const west =
        simulate(west_configuration, elevation, 64U);

    double east_side = 0.0;
    double west_side = 0.0;
    double east_rain = 0.0;
    double west_rain = 0.0;
    size_t samples = 0U;
    for (uint32_t y = 34U; y < 62U; ++y)
    {
        for (uint32_t x = 42U; x < 47U; ++x)
        {
            size_t const index = static_cast<size_t>(y) * RESOLUTION + x;
            east_side += east.precipitation[index];
            west_side += west.precipitation[index];
            ++samples;
        }
        for (uint32_t x = 49U; x < 54U; ++x)
        {
            size_t const index = static_cast<size_t>(y) * RESOLUTION + x;
            east_rain += east.precipitation[index];
            west_rain += west.precipitation[index];
        }
    }
    double const east_contrast = (east_side - east_rain) / static_cast<double>(samples);
    double const west_contrast = (west_rain - west_side) / static_cast<double>(samples);

    EXPECT_GT(east_contrast, 0.02);
    EXPECT_GT(west_contrast, 0.02);
    for (size_t index = 0U; index < elevation.size(); ++index)
    {
        EXPECT_FLOAT_EQ(east.wind_u[index], -west.wind_u[index]);
        EXPECT_FLOAT_EQ(east.wind_v[index], -west.wind_v[index]);
    }
}

TEST(ClimateTests, Seed42ProvisionalAndFinalFieldsMatchRetainedStageBytes)
{
    generation::GenerationConfiguration configuration;
    configuration.resolution = 256U;
    generation::detail::CoastalFields const coast = seed42Coast(configuration);
    generation::detail::HydrologyFields const initial = generation::detail::buildHydrology(
        configuration, coast.elevation, {}, {}, 0U, 0.0F,
        generation::GenerationStage::InitialDrainage, {}, {});
    generation::detail::ClimateFields const provisional = generation::detail::simulateClimate(
        configuration, coast.elevation, 24U, 0.0F,
        initial.ocean_mask, initial.lake_mask,
        generation::GenerationStage::ProvisionalClimate, {}, {});
    expectFixtureSamples("stage__provisional_climate_temperature", provisional.temperature);
    expectFixtureSamples("stage__provisional_climate_precipitation", provisional.precipitation);
    expectFixtureSamples("stage__provisional_climate_moisture", provisional.moisture);

    generation::detail::HydrologyFields const eroded = generation::detail::buildHydrology(
        configuration, coast.elevation, provisional.precipitation, provisional.temperature,
        3U, 0.0F, generation::GenerationStage::TerrainErosion, {}, {});
    generation::detail::ClimateFields const final = generation::detail::simulateClimate(
        configuration, eroded.elevation, 80U, 0.0F,
        eroded.ocean_mask, eroded.lake_mask,
        generation::GenerationStage::FinalClimate, {}, {});
    expectFixtureSamples("stage__final_climate_temperature", final.temperature);
    expectFixtureSamples("stage__final_climate_precipitation", final.precipitation);
    expectFixtureSamples("stage__final_climate_moisture", final.moisture);
}

} // namespace
