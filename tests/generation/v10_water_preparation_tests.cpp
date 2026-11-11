#include "V10WaterPreparation.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace
{
using namespace generation::detail;

TEST(V10WaterPreparation, RejectsIncompleteL0BeforeClimateAccess)
{
    std::vector<double> const bed(65'535U, 0.0);
    auto const result = prepareV10Water(bed, {}, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, V9WaterFailure::Kind::InvalidInput);
}

TEST(V10WaterPreparation, RejectsNonfiniteL0)
{
    std::vector<double> bed(65'536U, 0.0);
    bed[32'768U] = std::numeric_limits<double>::quiet_NaN();
    auto const result = prepareV10Water(bed, {}, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, V9WaterFailure::Kind::InvalidInput);
}

TEST(V10WaterPreparation, HonorsCancellationBeforeClimateAccess)
{
    std::vector<double> const bed(65'536U, 0.0);
    std::stop_source cancellation;
    cancellation.request_stop();
    auto const result = prepareV10Water(bed, {}, {}, cancellation.get_token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, V9WaterFailure::Kind::Cancelled);
}

TEST(V10WaterPreparation, RejectsMissingFreshClimateField)
{
    std::vector<double> const bed(65'536U, 0.0);
    auto const result = prepareV10Water(bed, {}, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, V9WaterFailure::Kind::InvalidInput);
    EXPECT_NE(result.error().message.find("ground_m"), std::string::npos);
}

TEST(V10WaterPreparation, RejectsShapeWithIncompleteValues)
{
    std::vector<double> const bed(65'536U, 0.0);
    V9ClimateResult climate;
    climate.fields.emplace("ground_m", V9ClimateField{{32U, 32U}, std::vector<double>(1023U)});
    auto const result = prepareV10Water(bed, climate, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, V9WaterFailure::Kind::InvalidInput);
    EXPECT_NE(result.error().message.find("ground_m"), std::string::npos);
}

TEST(V10WaterPreparation, CopiesLowerHumidityLayerAndNativeOceanFaceShape)
{
    V9ClimateResult climate;
    auto add = [&](char const* name, std::vector<uint32_t> shape, double value) {
        size_t count = 1U;
        for (auto extent : shape)
        {
            count *= extent;
        }
        climate.fields.emplace(name, V9ClimateField{std::move(shape), std::vector<double>(count, value)});
    };
    add("ground_m", {32U, 32U}, 0.0);
    add("reference_ps", {32U, 32U}, 100'000.0);
    add("wet_fraction", {32U, 32U}, 1.0);
    for (auto name : {"local_effective_C", "theta_C"})
    {
        add(name, {8U, 32U, 32U}, 10.0);
    }
    for (auto name : {"rain_kg_m2_s", "runoff_kg_m2_s", "u", "v", "ocean_east_volume",
        "ocean_north_volume", "ocean_evap_kg_m2_s"})
    {
        add(name, {8U, 32U, 32U}, 0.0);
    }
    add("layer_vapor_RH", {8U, 2U, 32U, 32U}, std::numeric_limits<double>::quiet_NaN());
    auto& rh = climate.fields.at("layer_vapor_RH").values;
    for (size_t phase = 0U; phase < 8U; ++phase)
    {
        std::fill_n(rh.begin() + static_cast<std::vector<double>::difference_type>(phase * 2048U), 1024U, .5);
    }
    // Deliberately poison the unused upper layer: selecting it instead of the
    // contract's lower humidity must make this otherwise valid input fail.
    V10FoundationBasis const foundation{std::vector<double>(65'536U, 10.0),
        std::vector<double>(65'536U, 10.0), std::vector<double>(65'536U, -256.0),
        std::vector<double>(65'536U, 0.0)};
    std::vector<double> const bed(65'536U, sampleV10FoundationHeight(foundation, 0.0, 0.0));
    auto const result = prepareV10Water(bed, climate, foundation);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->water.phase_count, 8U);
    EXPECT_EQ(result->fixed_support.size, 512U);
    EXPECT_EQ(result->water.equilibrium.ocean.size(), 65'536U);
}

TEST(V10WaterPreparation, PointOccupancyClipsSameWaterAuthorityAgainstActualBed)
{
    // Synthetic all-marine same-producer fixture: no categorical interpolation
    // can make the raised point wet merely because its support cell is ocean.
    std::vector<double> const bed(64U, -10.0);
    V9WaterRates const rates{std::vector<double>(64U), std::vector<double>(64U), std::vector<double>(64U)};
    auto equilibrium = solveV9WaterEquilibrium(bed, 8U, rates);
    ASSERT_TRUE(equilibrium) << equilibrium.error().message;
    auto routing = routeV9Water(*equilibrium);
    ASSERT_TRUE(routing) << routing.error().message;
    auto refinement = V9WaterRefinement::create(*equilibrium, *routing);
    ASSERT_TRUE(refinement) << refinement.error().message;
    auto fixed = buildV9WaterFixedSupport(*equilibrium);
    ASSERT_TRUE(fixed) << fixed.error().message;
    V9WaterResult water;
    water.equilibrium = std::move(*equilibrium);
    water.routing = std::move(*routing);
    V10WaterPreparation const prepared{std::move(water), std::move(*refinement), std::move(*fixed)};
    auto const below = queryV10WaterPoint(prepared, 1234.0, 4567.0, -2.5);
    auto const above = queryV10WaterPoint(prepared, 1234.0, 4567.0, 2.5);
    ASSERT_TRUE(below);
    ASSERT_TRUE(above);
    EXPECT_TRUE(below->ocean);
    EXPECT_FLOAT_EQ(below->water_level_m, 0.0F);
    EXPECT_FLOAT_EQ(below->water_depth_m, 2.5F);
    EXPECT_FALSE(above->ocean);
    EXPECT_FALSE(above->lake);
    EXPECT_FALSE(above->river);
    EXPECT_FLOAT_EQ(above->water_depth_m, 0.0F);
    auto const wrapped = queryV10WaterPoint(prepared, 1234.0 - 65'536.0, 4567.0 + 65'536.0, -2.5);
    ASSERT_TRUE(wrapped);
    EXPECT_EQ(wrapped->ocean, below->ocean);
    EXPECT_EQ(wrapped->water_depth_m, below->water_depth_m);
}

} // namespace
