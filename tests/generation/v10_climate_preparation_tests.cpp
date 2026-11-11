#include "V10ClimatePreparation.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef MC_V10_CLIMATE_FIXTURE_DIRECTORY
#define MC_V10_CLIMATE_FIXTURE_DIRECTORY ""
#endif

namespace
{
using namespace generation::detail;

std::vector<double> readDoubles(std::string const& name, size_t count)
{
    std::filesystem::path const directory(MC_V10_CLIMATE_FIXTURE_DIRECTORY);
    std::ifstream input(directory / (name + ".f64"), std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("cannot open V10 climate qualification fixture");
    }
    std::vector<double> values(count);
    input.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(count * sizeof(double)));
    if (!input || input.peek() != std::char_traits<char>::eof())
    {
        throw std::runtime_error("V10 climate qualification fixture has incorrect size");
    }
    return values;
}

TEST(V10ClimatePreparation, RejectsOtherSeedBeforeRunningSolver)
{
    std::vector<double> const bed(65'536U, 0.0);
    auto const result = prepareV10Climate(bed, 43U);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), V9ClimateFailure::InvalidInput);
}

TEST(V10ClimatePreparation, RejectsIncompleteCanonicalL0)
{
    std::vector<double> const bed(65'535U, 0.0);
    auto const result = prepareV10Climate(bed, 42U);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), V9ClimateFailure::InvalidInput);
}

TEST(V10ClimatePreparation, RejectsNonfiniteL0)
{
    std::vector<double> bed(65'536U, 0.0);
    bed[32'768U] = std::numeric_limits<double>::quiet_NaN();
    auto const result = prepareV10Climate(bed, 42U);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), V9ClimateFailure::InvalidInput);
}

TEST(V10ClimatePreparation, RejectsOverflowingClimateMetric)
{
    std::vector<double> bed(65'536U, 0.0);
    bed[32'768U] = std::numeric_limits<double>::max();
    auto const result = prepareV10Climate(bed, 42U);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), V9ClimateFailure::InvalidInput);
}

TEST(V10ClimatePreparation, HonorsCancellationBeforeRunningSolver)
{
    std::vector<double> const bed(65'536U, 0.0);
    std::stop_source cancellation;
    cancellation.request_stop();
    auto const result = prepareV10Climate(bed, 42U, cancellation.get_token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), V9ClimateFailure::Cancelled);
}

TEST(V10ClimatePreparation, FreshL0MatchesFrozenThermalAndPreservesRainRegistration)
{
    if (std::string(MC_V10_CLIMATE_FIXTURE_DIRECTORY).empty())
    {
        GTEST_SKIP() << "explicit source-bound V10 climate qualification fixtures required";
    }
    auto const bed = readDoubles("bed", 65'536U);
    auto const result = prepareV10Climate(bed, 42U);
    ASSERT_TRUE(result);
    auto const& fields = result->forcing.fields();
    auto compare = [](std::vector<double> const& actual, std::string const& name, double tolerance) {
        auto const expected = readDoubles(name, actual.size());
        double maximum_error = 0.0;
        for (size_t i = 0U; i < actual.size(); ++i)
        {
            maximum_error = std::max(maximum_error, std::abs(actual[i] - expected[i]));
        }
        EXPECT_LE(maximum_error, tolerance) << name;
    };
    compare(fields.ground_m, "ground", 1e-9);
    compare(fields.theta_celsius, "theta", 1e-6);
    compare(fields.local_effective_celsius, "air", 1e-6);
    // A climate cell centre is also a rain-grid vertex at (8*c+4).
    // The registered annual rain at that point must preserve the fresh source
    // rate and the water export's explicit float32 storage, phase by phase.
    auto const& source_rain = result->climate.fields.at("rain_kg_m2_s").values;
    for (size_t phase = 0U; phase < 8U; ++phase)
    {
        for (size_t y = 0U; y < 32U; ++y)
        {
            for (size_t x = 0U; x < 32U; ++x)
            {
                double const expected = static_cast<float>(source_rain[(phase * 32U + y) * 32U + x]
                    * (360.0 * 86'400.0));
                EXPECT_EQ(fields.precipitation_mm_year[(phase * 256U + y * 8U + 4U) * 256U + x * 8U + 4U],
                    expected);
            }
        }
    }
    auto const summit = queryV10Climate(result->forcing, 46'336.0, 9984.0, bed[39U * 256U + 181U]);
    EXPECT_EQ(summit.biome_id, 0U);
    EXPECT_GT(summit.mean_glacier_thickness_m, 0.0);
}

} // namespace
