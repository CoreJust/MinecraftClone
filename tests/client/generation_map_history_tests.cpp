#include <client/GenerationMapHistory.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace
{
struct Source final
{
    uint32_t side = 64U;
    std::vector<double> height = std::vector<double>(side * side, -100.0);
    std::vector<double> water = std::vector<double>(side * side, 0.0);
    std::vector<uint32_t> plates = std::vector<uint32_t>(side * side, 1U);
    client::GenerationHistorySource view() const { return {side, height, water, plates, 0.0}; }
};
size_t bodies(std::vector<double> const& height, uint32_t side)
{
    std::vector<bool> seen(height.size()); size_t count = 0U;
    for (uint32_t i = 0U; i < height.size(); ++i)
    {
        if (seen[i] || height[i] <= 0.0) continue;
        ++count; std::queue<uint32_t> queue; queue.push(i); seen[i] = true;
        while (!queue.empty())
        {
            uint32_t const p = queue.front(); queue.pop(); uint32_t const x = p % side, y = p / side;
            for (uint32_t n : {y * side + (x + 1U) % side, y * side + (x + side - 1U) % side,
                ((y + 1U) % side) * side + x, ((y + side - 1U) % side) * side + x})
                if (!seen[n] && height[n] > 0.0) { seen[n] = true; queue.push(n); }
        }
    }
    return count;
}
TEST(GenerationMapHistoryTest, StartsEntirelyMoltenAndAlwaysHasACompleteFiniteSurface)
{
    Source source; source.height[20U * 64U + 20U] = 1000.0;
    auto history = client::GenerationMapHistory::prepare(source.view()); ASSERT_TRUE(history);
    client::GenerationHistoryFrame frame;
    ASSERT_TRUE(history->evaluate(0.0, frame));
    EXPECT_TRUE(std::all_of(frame.liquid_fraction.begin(), frame.liquid_fraction.end(), [](float x) { return x == 1.0F; }));
    for (double t : {0.0, .1, .3, .5, .7, 1.0})
    {
        ASSERT_TRUE(history->evaluate(t, frame)); EXPECT_EQ(frame.elevation_m.size(), source.height.size());
        EXPECT_TRUE(std::all_of(frame.elevation_m.begin(), frame.elevation_m.end(), [](double x) { return std::isfinite(x); }));
    }
}
TEST(GenerationMapHistoryTest, FinalFieldsAndEndpointDerivativesAreExact)
{
    Source source; for (uint32_t i = 0; i < source.height.size(); ++i) source.height[i] = static_cast<double>(i % 123U) - 80.0;
    auto history = client::GenerationMapHistory::prepare(source.view()); ASSERT_TRUE(history);
    client::GenerationHistoryFrame frame;
    for (double t : {.999, 1.0})
    {
        ASSERT_TRUE(history->evaluate(t, frame)); EXPECT_EQ(frame.elevation_m, source.height);
        for (size_t i = 0; i < source.height.size(); ++i)
        {
            EXPECT_EQ(frame.liquid_fraction[i], 0.0F); EXPECT_EQ(frame.boundary_heat[i], 0.0F);
            EXPECT_FLOAT_EQ(frame.water_depth_m[i], static_cast<float>(std::max(0.0, -source.height[i])));
        }
    }
}
TEST(GenerationMapHistoryTest, ScalarSeaCrossingsMergeAndSplitLandWithoutDeletingCells)
{
    Source source; std::vector<double> bridge(source.height.size());
    for (uint32_t y = 25; y <= 35; ++y) for (uint32_t x = 15; x <= 40; ++x)
    {
        if (x <= 23 || x >= 32) source.height[y * 64 + x] = 200.0;
        else bridge[y * 64 + x] = 500.0;
    }
    client::GenerationHistoryEvent const event{bridge, .25, .75};
    auto history = client::GenerationMapHistory::prepare(source.view(), {}, {&event, 1U}); ASSERT_TRUE(history);
    client::GenerationHistoryFrame frame;
    ASSERT_TRUE(history->evaluate(.2, frame)); EXPECT_EQ(bodies(frame.elevation_m, 64), 2U);
    ASSERT_TRUE(history->evaluate(.5, frame)); EXPECT_EQ(bodies(frame.elevation_m, 64), 1U);
    ASSERT_TRUE(history->evaluate(.9, frame)); EXPECT_EQ(bodies(frame.elevation_m, 64), 2U);
}
TEST(GenerationMapHistoryTest, PlateBordersStayHotAfterRaisedInteriorsSolidify)
{
    Source source; std::fill(source.height.begin(), source.height.end(), 100.0);
    for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 32; x < 64; ++x) source.plates[y * 64 + x] = 2U;
    auto history = client::GenerationMapHistory::prepare(source.view()); ASSERT_TRUE(history);
    client::GenerationHistoryFrame frame; ASSERT_TRUE(history->evaluate(.4, frame));
    EXPECT_GT(frame.liquid_fraction[32U * 64U + 31U], frame.liquid_fraction[32U * 64U + 16U]);
    EXPECT_GT(frame.boundary_heat[32U * 64U + 31U], .1F);
}
TEST(GenerationMapHistoryTest, RejectsInvalidSourcesCoresAndEvents)
{
    Source source;
    auto invalid = source.view();
    invalid.elevation_m = {};
    EXPECT_FALSE(client::GenerationMapHistory::prepare(invalid));
    source.height[0] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(client::GenerationMapHistory::prepare(source.view()));
    source.height[0] = -100.0;
    client::GenerationHistoryCore const core{{16,16},{32,16},8,4,0};
    EXPECT_FALSE(client::GenerationMapHistory::prepare(source.view(), {&core,1U}));
    client::GenerationHistoryEvent const event{source.height,.8,.2};
    EXPECT_FALSE(client::GenerationMapHistory::prepare(source.view(), {}, {&event,1U}));
}
TEST(GenerationMapHistoryTest, InvalidTimeDoesNotReplaceThePreviousFrame)
{
    Source source; auto history = client::GenerationMapHistory::prepare(source.view()); ASSERT_TRUE(history);
    client::GenerationHistoryFrame frame; ASSERT_TRUE(history->evaluate(.5, frame)); auto previous = frame.elevation_m;
    EXPECT_FALSE(history->evaluate(std::numeric_limits<double>::quiet_NaN(), frame)); EXPECT_EQ(frame.elevation_m, previous);
}
TEST(GenerationMapHistoryTest, RotatingAcrossPeriodicSeamsKeepsCoordinatesAndThermalFieldsValid)
{
    Source source;
    client::GenerationHistoryCore const core{{0.0, 0.0}, {0.0, 0.0}, 8.0, 12.0, 1e-9};
    auto history = client::GenerationMapHistory::prepare(source.view(), {&core, 1U});
    ASSERT_TRUE(history);
    client::GenerationHistoryFrame frame;
    for (double time : {0.0, .2, .4, .6, .8, 1.0})
    {
        ASSERT_TRUE(history->evaluate(time, frame));
        for (size_t index = 0U; index < frame.elevation_m.size(); ++index)
        {
            auto const coordinates = frame.final_coordinates[index];
            ASSERT_GE(coordinates[0U], 0.0F);
            ASSERT_LT(coordinates[0U], source.side);
            ASSERT_GE(coordinates[1U], 0.0F);
            ASSERT_LT(coordinates[1U], source.side);
            ASSERT_TRUE(std::isfinite(frame.elevation_m[index]));
            ASSERT_TRUE(std::isfinite(frame.water_depth_m[index]));
            ASSERT_GE(frame.water_depth_m[index], 0.0F);
            ASSERT_GE(frame.liquid_fraction[index], 0.0F);
            ASSERT_LE(frame.liquid_fraction[index], 1.0F);
            ASSERT_GE(frame.boundary_heat[index], 0.0F);
            ASSERT_LE(frame.boundary_heat[index], 1.0F);
        }
    }
}
TEST(GenerationMapHistoryTest, VacatedIslandDoesNotRemainRaisedWhileItsRigidBodyIsAway)
{
    Source source;
    for (uint32_t y = 0U; y < 64U; ++y) for (uint32_t x = 0U; x < 64U; ++x)
        if (std::hypot(static_cast<double>(x) - 16.0, static_cast<double>(y) - 16.0) < 4.5)
            source.height[y * 64U + x] = 500.0;
    auto original = source.height;
    client::GenerationHistoryCore const core{{16.0,16.0},{32.0,16.0},8.0,12.0,0.0};
    auto history = client::GenerationMapHistory::prepare(source.view(), {&core,1U}); ASSERT_TRUE(history);
    client::GenerationHistoryFrame frame;
    for (double time : {.25,.4,.5})
    {
        ASSERT_TRUE(history->evaluate(time,frame));
        EXPECT_DOUBLE_EQ(frame.elevation_m[16U * 64U + 16U], -100.0);
        EXPECT_DOUBLE_EQ(frame.water_level_m[16U * 64U + 16U], 0.0);
        EXPECT_EQ(bodies(frame.elevation_m,64U),1U);
        EXPECT_DOUBLE_EQ(frame.elevation_m[0U], -100.0);
    }
    ASSERT_TRUE(history->evaluate(1.0,frame)); EXPECT_EQ(frame.elevation_m,original);
    EXPECT_EQ(source.height,original);
}
TEST(GenerationMapHistoryTest, IndependentlyRotatingSolidCoreKeepsItsLocalShape)
{
    Source source;
    for (uint32_t y = 0U; y < 64U; ++y) for (uint32_t x = 0U; x < 64U; ++x)
        if (std::hypot(static_cast<double>(x) - 16.0, static_cast<double>(y) - 16.0) < 5.0)
            source.height[y * 64U + x] = 500.0 + 3.0 * (x - 16.0) - 2.0 * (y - 16.0);
    client::GenerationHistoryCore const core{{16.0,16.0},{32.0,16.0},8.0,12.0,1.570'796'326'794'896'6};
    auto history = client::GenerationMapHistory::prepare(source.view(), {&core,1U}); ASSERT_TRUE(history);
    client::GenerationHistoryFrame frame;
    auto periodic_delta = [](float a,float b) { double d = a-b; return d-64.0*std::round(d/64.0); };
    double first_rotation = 0.0;
    for (double time : {.25,.5,.7})
    {
        ASSERT_TRUE(history->evaluate(time,frame));
        size_t center = 0U; double nearest = std::numeric_limits<double>::max();
        for (size_t i = 0U; i < frame.elevation_m.size(); ++i)
            if (frame.core_weight[i] == 1.0F)
            {
                auto uv=frame.final_coordinates[i]; double d=std::hypot(uv[0]-16.0,uv[1]-16.0);
                if(d<nearest){nearest=d;center=i;}
            }
        ASSERT_LT(nearest,1.0);
        uint32_t x=static_cast<uint32_t>(center)%64U,y=static_cast<uint32_t>(center)/64U;
        auto east=frame.final_coordinates[y*64U+(x+1U)%64U], west=frame.final_coordinates[y*64U+(x+63U)%64U];
        auto south=frame.final_coordinates[((y+1U)%64U)*64U+x],north=frame.final_coordinates[((y+63U)%64U)*64U+x];
        double ax=periodic_delta(east[0],west[0])*.5, ay=periodic_delta(east[1],west[1])*.5;
        double bx=periodic_delta(south[0],north[0])*.5,by=periodic_delta(south[1],north[1])*.5;
        EXPECT_NEAR(ax*ax+ay*ay,1.0,1e-5); EXPECT_NEAR(bx*bx+by*by,1.0,1e-5);
        EXPECT_NEAR(ax*bx+ay*by,0.0,1e-5); EXPECT_NEAR(ax*by-ay*bx,1.0,1e-5);
        auto uv=frame.final_coordinates[center];
        EXPECT_NEAR(frame.elevation_m[center],500.0+3.0*(uv[0]-16.0)-2.0*(uv[1]-16.0),.001);
        if(time==.25){first_rotation=std::abs(ay);EXPECT_GT(first_rotation,.5);}
        if(time==.7)EXPECT_LT(std::abs(ay),first_rotation*.2);
    }
}
TEST(GenerationMapHistoryTest, RejectsUnrepresentableFiniteSourceAndCoreInputs)
{
    Source source;
    double const extreme = std::numeric_limits<double>::max();
    source.height[0U] = -extreme;
    source.water[0U] = extreme;
    EXPECT_FALSE(client::GenerationMapHistory::prepare(source.view()));
    source.height[0U] = -100.0;
    EXPECT_FALSE(client::GenerationMapHistory::prepare(source.view()));
    source.water[0U] = 0.0;
    auto view = source.view();
    view.sea_level_m = extreme;
    EXPECT_FALSE(client::GenerationMapHistory::prepare(view));
    client::GenerationHistoryCore core{{16.0,16.0},{32.0,16.0},8.0,12.0,0.0};
    core.initial_rotation_radians = extreme;
    EXPECT_FALSE(client::GenerationMapHistory::prepare(source.view(), {&core,1U}));
}
TEST(GenerationMapHistoryTest, RejectsUnrepresentableFiniteEventOffsets)
{
    Source source;
    std::vector<double> offsets(source.height.size(), 0.0);
    offsets[0U] = std::numeric_limits<double>::max();
    client::GenerationHistoryEvent event{offsets,.25,.75};
    EXPECT_FALSE(client::GenerationMapHistory::prepare(source.view(), {}, {&event,1U}));
    offsets[0U] = -std::numeric_limits<double>::max();
    EXPECT_FALSE(client::GenerationMapHistory::prepare(source.view(), {}, {&event,1U}));
}
TEST(GenerationMapHistoryTest, BoundedLargeFieldsAndSixteenEventsKeepEveryOutputFinite)
{
    static constexpr uint32_t SIDE = 4U;
    double const limit = static_cast<double>(std::numeric_limits<float>::max()) / (16.0 + 8.0);
    std::vector<double> heights(SIDE * SIDE, -limit), water(SIDE * SIDE, limit);
    std::vector<uint32_t> plates(SIDE * SIDE, 1U);
    std::vector<double> offsets(SIDE * SIDE, -limit);
    std::vector<client::GenerationHistoryEvent> events(16U, {offsets,.25,.75});
    for (uint32_t index = 1U; index < heights.size(); index += 2U) heights[index] = limit;
    for (double sea : {-limit, 0.0, limit})
    for (double water_value : {-limit, limit})
    for (double event_value : {-limit, limit})
    {
        std::fill(water.begin(), water.end(), water_value);
        std::fill(offsets.begin(), offsets.end(), event_value);
        client::GenerationHistorySource source{SIDE,heights,water,plates,sea};
        auto history = client::GenerationMapHistory::prepare(source, {}, events);
        ASSERT_TRUE(history);
        client::GenerationHistoryFrame frame;
        for (double time : {0.0,.25,.5,.75,1.0})
        {
            ASSERT_TRUE(history->evaluate(time, frame));
            auto finite_values = [](auto const& values) {
                return std::all_of(values.begin(), values.end(), [](auto value) { return std::isfinite(value); });
            };
            EXPECT_TRUE(finite_values(frame.elevation_m));
            EXPECT_TRUE(finite_values(frame.water_level_m));
            EXPECT_TRUE(finite_values(frame.water_depth_m));
            EXPECT_TRUE(finite_values(frame.liquid_fraction));
            EXPECT_TRUE(finite_values(frame.boundary_heat));
            EXPECT_TRUE(finite_values(frame.next_elevation_m));
            EXPECT_TRUE(finite_values(frame.warm_weight));
            EXPECT_TRUE(finite_values(frame.core_weight));
            EXPECT_TRUE(std::all_of(frame.final_coordinates.begin(), frame.final_coordinates.end(), [](auto uv) {
                return std::isfinite(uv[0U]) && std::isfinite(uv[1U])
                    && uv[0U] >= 0.0F && uv[0U] < SIDE && uv[1U] >= 0.0F && uv[1U] < SIDE;
            }));
        }
    }
}
} // namespace
