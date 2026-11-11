#include "V9Climate.hpp"
#include "V9ClimateQg.hpp"
#include "V9Numerics.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <stop_token>
#include <utility>
#include <vector>
using namespace generation::detail;
TEST(V9ClimateTests, RejectsWrongCanonicalBedShape)
{
    auto result = generateV9Climate(std::vector<double>(32 * 32), 42);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), V9ClimateFailure::InvalidInput);
}
TEST(V9ClimateTests, RejectsNonfiniteCanonicalBed)
{
    std::vector<double> bed(256 * 256);
    bed[177] = std::numeric_limits<double>::quiet_NaN();
    auto result = generateV9Climate(bed, 42);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), V9ClimateFailure::InvalidInput);
}
TEST(V9ClimateTests, CancellationDoesNotPublishPartialClimate)
{
    std::stop_source source;
    source.request_stop();
    uint32_t callbacks = 0;
    auto result = generateV9Climate(std::vector<double>(256 * 256), 42, source.get_token(),
                            [&](V9ClimateProgress) { ++callbacks; });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), V9ClimateFailure::Cancelled);
    EXPECT_EQ(callbacks, 0U);
}
TEST(V9ClimateTests, PortableTransformsRetainAsymmetricFields)
{
    std::vector<double> input(2 * 32 * 32);
    for (uint32_t l = 0; l < 2; ++l)
        for (uint32_t y = 0; y < 32; ++y)
            for (uint32_t x = 0; x < 32; ++x)
                input[l * 1024 + y * 32 + x] = .1 * l + std::sin(.17 * x + .23 * y) + .01 * x;
    for (bool sine : {false, true})
    {
        std::vector<std::complex<double>> transformed;
        std::vector<double> restored;
        v9Transform(input, transformed, sine);
        v9Inverse(std::move(transformed), restored, sine);
        ASSERT_EQ(restored.size(), input.size());
        for (uint32_t i = 0; i < input.size(); ++i)
            EXPECT_NEAR(restored[i], input[i], 3e-14);
    }
}
TEST(V9ClimateTests, ProjectedCarrierClosesPolarFacesAndDryMass)
{
    std::vector<double> fx(1024), fy(33 * 32);
    for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 32; ++x)
        {
            fx[y * 32 + x] = 17'000 * std::sin(2 * std::numbers::pi * x / 32) + 31 * y;
            fy[y * 32 + x] = 9000 * std::cos(.2 * y + .7 * x);
        }
    std::vector<double> px, py;
    projectV9Flux(fx, fy, 512'000., px, py);
    for (uint32_t x = 0; x < 32; ++x)
    {
        EXPECT_EQ(py[x], 0.);
        EXPECT_EQ(py[32 * 32 + x], 0.);
    }
    for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 32; ++x)
            EXPECT_NEAR((px[y * 32 + x] - px[y * 32 + (x + 31) % 32] + py[(y + 1) * 32 + x] -
                         py[y * 32 + x]) /
                            512'000.,
                        0., 1e-14);
}
TEST(V9ClimateTests, WeatherCancellationPrecedesRandomInitialization)
{
    std::stop_source source;
    source.request_stop();
    EXPECT_THROW((void)computeV9Weather(std::vector<double>(8192), std::vector<double>(1024), 42,
                            1000., source.get_token(), {}),
                 std::runtime_error);
}
TEST(V9ClimateTests, CancellationPropagatesThroughThermalStage)
{
    std::stop_source source;
    bool observed_thermal = false;
    bool published_complete = false;
    auto result = generateV9Climate(std::vector<double>(256 * 256), 42, source.get_token(),
                            [&](V9ClimateProgress progress)
                            {
                            if (progress.detail == "reference-capacity")
                            {
                            observed_thermal = true;
                            source.request_stop();
                            }
                            published_complete |=
                            progress.stage == V9ClimateProgress::Stage::Complete;
                            });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), V9ClimateFailure::Cancelled);
    EXPECT_TRUE(observed_thermal);
    EXPECT_FALSE(published_complete);
}
TEST(V9ClimateTests, FinalMoistureCancellationDoesNotPublishComplete)
{
    static constexpr uint32_t BED_RESOLUTION = 256;
    static constexpr uint64_t SEED = 42;
    std::stop_source source;
    bool observed_final_moisture = false;
    bool published_complete = false;
    std::vector<double> bed(BED_RESOLUTION * BED_RESOLUTION, 1000.);
    auto result = generateV9Climate(bed, SEED, source.get_token(),
                            [&](V9ClimateProgress progress)
                            {
                            if (progress.stage == V9ClimateProgress::Stage::Moisture &&
                            progress.completed == progress.total)
                            {
                            observed_final_moisture = true;
                            source.request_stop();
                            }
                            published_complete |=
                            progress.stage == V9ClimateProgress::Stage::Complete;
                            });
    EXPECT_TRUE(observed_final_moisture);
    EXPECT_FALSE(published_complete);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), V9ClimateFailure::Cancelled);
}
