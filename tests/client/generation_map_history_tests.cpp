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
std::vector<uint8_t> landMask(Source const& source)
{
    std::vector<uint8_t> mask(source.height.size());
    for (size_t index = 0U; index < mask.size(); ++index) mask[index] = source.height[index] > 0.0 ? 1U : 0U;
    return mask;
}
std::vector<uint8_t> smallMask(Source const& source)
{
    std::vector<uint8_t> mask(source.height.size());
    for (uint32_t y = 0U; y < 4U; ++y) for (uint32_t x = 0U; x < 4U; ++x)
        mask[((y + source.side - 2U) % source.side) * source.side + (x + source.side - 2U) % source.side] = 1U;
    return mask;
}
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
    auto mask = smallMask(source);
    client::GenerationHistoryCore const core{{16,16},{32,16},mask,-1.0,0};
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
    auto mask = smallMask(source);
    client::GenerationHistoryCore const core{{0.0, 0.0}, {0.0, 0.0}, mask, 3.0, 1e-9};
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
    auto mask = landMask(source);
    client::GenerationHistoryCore const core{{16.0,16.0},{32.0,16.0},mask,3.0,0.0};
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
    auto mask = landMask(source);
    client::GenerationHistoryCore const core{{16.0,16.0},{32.0,16.0},mask,3.0,1.570'796'326'794'896'6};
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
    auto mask = smallMask(source);
    client::GenerationHistoryCore core{{16.0,16.0},{32.0,16.0},mask,3.0,0.0};
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

TEST(GenerationMapHistoryTest, ActualFootprintsAreCopiedAndFinalOwnershipCannotOverlap)
{
    Source source;
    auto mask = smallMask(source);
    client::GenerationHistoryCore core{{0,0},{12,16},mask,3.0,.4};
    auto history = client::GenerationMapHistory::prepare(source.view(), {&core,1U}); ASSERT_TRUE(history);
    std::array duplicate{core,core};
    EXPECT_FALSE(client::GenerationMapHistory::prepare(source.view(),duplicate));
    mask[0] = 2U;
    EXPECT_FALSE(client::GenerationMapHistory::prepare(source.view(),{&core,1U}));
    std::fill(mask.begin(),mask.end(),0U);
    EXPECT_FALSE(client::GenerationMapHistory::prepare(source.view(),{&core,1U}));
    core.source_footprint = std::span<uint8_t const>{mask}.first(mask.size()-1U);
    EXPECT_FALSE(client::GenerationMapHistory::prepare(source.view(),{&core,1U}));
    client::GenerationHistoryFrame frame;
    ASSERT_TRUE(history->evaluate(.3,frame));
    EXPECT_GT(*std::max_element(frame.core_weight.begin(),frame.core_weight.end()),0.0F);
}

TEST(GenerationMapHistoryTest, SourceOrderDoesNotChangeIrregularBodiesOrTheirScalarOverlap)
{
    Source source;
    std::vector<uint8_t> a(source.height.size()),b(source.height.size());
    for(uint32_t y=8;y<19;++y) for(uint32_t x=8;x<18;++x)
        if(x<12||y<12) {a[y*64+x]=1U;source.height[y*64+x]=400.0+2*x-y;}
    for(uint32_t y=20;y<28;++y) for(uint32_t x=38;x<46;++x)
        {b[y*64+x]=1U;source.height[y*64+x]=650.0+x-2*y;}
    std::array cores{client::GenerationHistoryCore{{12,12},{20,10},a,3.0,.35},
        client::GenerationHistoryCore{{42,24},{-10,-2},b,3.0,-.4}};
    auto first=client::GenerationMapHistory::prepare(source.view(),cores);ASSERT_TRUE(first);
    std::reverse(cores.begin(),cores.end());
    auto second=client::GenerationMapHistory::prepare(source.view(),cores);ASSERT_TRUE(second);
    client::GenerationHistoryFrame left,right;
    bool saw_overlap=false;
    for(double time:{.2,.35,.5,.7,.779,1.0})
    {
        ASSERT_TRUE(first->evaluate(time,left));ASSERT_TRUE(second->evaluate(time,right));
        EXPECT_EQ(left.elevation_m,right.elevation_m);EXPECT_EQ(left.water_level_m,right.water_level_m);
        EXPECT_EQ(left.overlap_fraction,right.overlap_fraction);EXPECT_EQ(left.final_coordinates,right.final_coordinates);
        EXPECT_EQ(left.source_body_id,right.source_body_id);EXPECT_EQ(left.source_mapping_weight,right.source_mapping_weight);
        for(size_t i=0;i<left.overlap_fraction.size();++i)
            if(left.overlap_fraction[i]>0.0F)
            {saw_overlap=true;EXPECT_EQ(left.source_body_id[i],0U);EXPECT_GE(left.liquid_fraction[i],left.overlap_fraction[i]);}
    }
    EXPECT_TRUE(saw_overlap);EXPECT_EQ(left.elevation_m,source.height);EXPECT_EQ(left.water_level_m,source.water);
    for(size_t i=0;i<left.final_coordinates.size();++i)
    {
        EXPECT_EQ(left.final_coordinates[i][0],static_cast<float>(i%64));
        EXPECT_EQ(left.final_coordinates[i][1],static_cast<float>(i/64));
    }
}

TEST(GenerationMapHistoryTest, ATransportedLakeRetainsItsBedAndStageInsteadOfBeingFilledByBackground)
{
    Source source;std::vector<uint8_t> mask(source.height.size());
    for(uint32_t y=10;y<17;++y)for(uint32_t x=10;x<17;++x)
        {mask[y*64+x]=1U;source.height[y*64+x]=-250.0;source.water[y*64+x]=50.0;}
    client::GenerationHistoryCore core{{13,13},{24,20},mask,3.0,.3};
    auto history=client::GenerationMapHistory::prepare(source.view(),{&core,1U});ASSERT_TRUE(history);
    client::GenerationHistoryFrame frame;ASSERT_TRUE(history->evaluate(.3,frame));
    size_t rigid=0;
    for(size_t i=0;i<mask.size();++i)if(frame.core_weight[i]==1.0F)
    {++rigid;EXPECT_NEAR(frame.elevation_m[i],-250.0,1e-10);EXPECT_NEAR(frame.water_level_m[i],50.0,1e-10);}
    EXPECT_GT(rigid,0U);EXPECT_DOUBLE_EQ(frame.elevation_m[0],-100.0);
    ASSERT_TRUE(history->evaluate(1.0,frame));
    EXPECT_EQ(frame.elevation_m,source.height);
    EXPECT_EQ(frame.water_level_m,source.water);
}

TEST(GenerationMapHistoryTest, HotOverlapHasContinuousHeightWithoutASupportWinnerSwitch)
{
    Source source;std::vector<uint8_t>a(source.height.size()),b(source.height.size());
    for(uint32_t y=20;y<30;++y)for(uint32_t x=8;x<18;++x){a[y*64+x]=1;source.height[y*64+x]=300;}
    for(uint32_t y=20;y<30;++y)for(uint32_t x=40;x<50;++x){b[y*64+x]=1;source.height[y*64+x]=800;}
    std::array cores{client::GenerationHistoryCore{{13,25},{16,0},a,3.0,.2},
        client::GenerationHistoryCore{{45,25},{-16,0},b,3.0,-.2}};
    auto history=client::GenerationMapHistory::prepare(source.view(),cores);ASSERT_TRUE(history);
    client::GenerationHistoryFrame before,after;
    ASSERT_TRUE(history->evaluate(.35-1e-6,before));ASSERT_TRUE(history->evaluate(.35+1e-6,after));
    double difference=0;
    for(size_t i=0;i<a.size();++i)difference=std::max(difference,std::abs(before.elevation_m[i]-after.elevation_m[i]));
    EXPECT_LT(difference,1.0);
    EXPECT_GT(*std::max_element(before.overlap_fraction.begin(),before.overlap_fraction.end()),.5F);
}

TEST(GenerationMapHistoryTest, PeriodicFootprintAndShiftedCentersDescribeTheSameTransport)
{
    Source source;auto mask=smallMask(source);
    for(size_t i=0;i<mask.size();++i)if(mask[i])source.height[i]=500;
    client::GenerationHistoryCore core{{0,0},{30,31},mask,3.0,.7};
    auto first=client::GenerationMapHistory::prepare(source.view(),{&core,1U});ASSERT_TRUE(first);
    core.source_center={64,64};
    auto second=client::GenerationMapHistory::prepare(source.view(),{&core,1U});ASSERT_TRUE(second);
    client::GenerationHistoryFrame a,b;
    for(double t:{.2,.35,.6,.779,1.0})
    {
        ASSERT_TRUE(first->evaluate(t,a));
        ASSERT_TRUE(second->evaluate(t,b));
        EXPECT_EQ(a.elevation_m,b.elevation_m);
        EXPECT_EQ(a.final_coordinates,b.final_coordinates);
    }
}

TEST(GenerationMapHistoryTest, SixtyFourBodiesAndSixteenEventsRetainTheSupportedNumericDomain)
{
    constexpr uint32_t SIDE=16U;constexpr size_t COUNT=SIDE*SIDE;
    double const limit=static_cast<double>(std::numeric_limits<float>::max())/24.0;
    std::vector<double>height(COUNT,-limit),water(COUNT,limit),offset(COUNT,-limit);
    std::vector<uint32_t>plates(COUNT,1U);std::vector<std::vector<uint8_t>>masks(64U,std::vector<uint8_t>(COUNT));
    std::vector<client::GenerationHistoryCore>cores;
    for(uint32_t i=0;i<64;++i)
    {height[i]=limit;masks[i][i]=1U;double x=i%SIDE,y=i/SIDE;cores.push_back({{x,y},{8-x,8-y},masks[i],3.0,.2});}
    std::vector<client::GenerationHistoryEvent>events(16U,{offset,.25,.75});
    auto history=client::GenerationMapHistory::prepare({SIDE,height,water,plates,0.0},cores,events);ASSERT_TRUE(history);
    client::GenerationHistoryFrame frame;
    for(double t:{0.0,.25,.5,.779,1.0})
    {
        ASSERT_TRUE(history->evaluate(t,frame));
        for(size_t i=0;i<COUNT;++i)
        {EXPECT_TRUE(std::isfinite(frame.elevation_m[i]));EXPECT_TRUE(std::isfinite(frame.water_level_m[i]));
         EXPECT_TRUE(std::isfinite(frame.water_depth_m[i]));EXPECT_TRUE(std::isfinite(frame.overlap_fraction[i]));
         EXPECT_GE(frame.overlap_fraction[i],0.0F);EXPECT_LE(frame.overlap_fraction[i],1.0F);
         EXPECT_TRUE(std::isfinite(frame.source_mapping_weight[i]));
         EXPECT_GE(frame.source_mapping_weight[i],0.0F);EXPECT_LE(frame.source_mapping_weight[i],1.0F);}
    }
}
TEST(GenerationMapHistoryTest, VacatedWaterUsesOutsideBathymetryEvenWhenAllHeightsAreFlat)
{
    Source source; std::vector<uint8_t> mask(source.height.size());
    for (uint32_t y = 0U; y < source.side; ++y)
        for (uint32_t x = 0U; x < source.side; ++x) source.water[y * source.side + x] = x;
    for (uint32_t y = 10U; y < 17U; ++y)
        for (uint32_t x = 10U; x < 17U; ++x) mask[y * source.side + x] = 1U;
    client::GenerationHistoryCore const core{{13.0, 13.0}, {24.0, 20.0}, mask, 3.0, 0.0};
    auto history = client::GenerationMapHistory::prepare(source.view(), {&core, 1U}); ASSERT_TRUE(history);
    client::GenerationHistoryFrame frame; ASSERT_TRUE(history->evaluate(.2, frame));
    EXPECT_NEAR(frame.water_level_m[13U * source.side + 12U], 12.0, 1.0e-5);
    EXPECT_DOUBLE_EQ(frame.elevation_m[13U * source.side + 12U], -100.0);
}

TEST(GenerationMapHistoryTest, AmbiguousPeriodicProvenanceFadesContinuouslyWithoutAColorWinner)
{
    Source source; std::vector<uint8_t> a(source.height.size()), b(source.height.size());
    a[8U * 64U + 8U] = 1U; b[8U * 64U + 40U] = 1U;
    source.height[8U * 64U + 8U] = 400.0; source.height[8U * 64U + 40U] = 700.0;
    std::array cores{client::GenerationHistoryCore{{8.0, 8.0}, {24.5, 16.0}, a, 3.0, 0.0},
        client::GenerationHistoryCore{{40.0, 8.0}, {-7.5, 16.0}, b, 3.0, 0.0}};
    client::GenerationHistoryFrame frame;
    for (double epsilon : {-1.0e-6, 0.0, 1.0e-6})
    {
        cores[1].initial_displacement_cells[0] = -7.5 + epsilon;
        auto history = client::GenerationMapHistory::prepare(source.view(), cores); ASSERT_TRUE(history);
        ASSERT_TRUE(history->evaluate(.18, frame));
        size_t const index = 24U * 64U + 32U;
        EXPECT_EQ(frame.source_body_id[index], 0U);
        EXPECT_NEAR(frame.overlap_fraction[index], .5F, 2.0e-6F);
        EXPECT_LT(frame.source_mapping_weight[index], 1.0e-6F);
    }
}

TEST(GenerationMapHistoryTest, SubUlpNegativeCoordinatesWrapToZeroWithoutIndexingPastTheGrid)
{
    Source source; auto mask = smallMask(source);
    for (size_t index = 0U; index < source.height.size(); ++index) source.height[index] = index;
    client::GenerationHistoryCore const core{{0.0, 0.0}, {0.0, 0.0}, mask, 3.0, 1.0e-18};
    auto history = client::GenerationMapHistory::prepare(source.view(), {&core, 1U}); ASSERT_TRUE(history);
    client::GenerationHistoryFrame frame; ASSERT_TRUE(history->evaluate(.2, frame));
    EXPECT_NEAR(frame.elevation_m[63U * 64U], source.height[63U * 64U], 1.0e-9);
    for (auto const& uv : frame.final_coordinates)
    { EXPECT_GE(uv[0], 0.0F); EXPECT_LT(uv[0], 64.0F); EXPECT_GE(uv[1], 0.0F); EXPECT_LT(uv[1], 64.0F); }
}

} // namespace

TEST(GenerationMapHistoryTest, EveryOverlappingBodyContributesToHeightAndWater)
{
    constexpr uint32_t SIDE = 64U;
    constexpr size_t COUNT = SIDE * SIDE;
    std::vector<double> height(COUNT, 0.0), water(COUNT, 0.0);
    std::vector<uint32_t> plates(COUNT, 1U);
    std::vector<uint8_t> a(COUNT), b(COUNT);
    for (uint32_t y = 20U; y < 24U; ++y)
    for (uint32_t x = 8U; x < 12U; ++x)
    {
        size_t const first = y * SIDE + x, second = first + 32U;
        a[first] = 1U; b[second] = 1U;
        height[first] = 100.0; height[second] = 200.0;
        water[first] = 20.0; water[second] = 40.0;
    }
    std::array cores{client::GenerationHistoryCore{{9.5, 21.5}, {16.0, 0.0}, a, 3.0, 0.0},
        client::GenerationHistoryCore{{41.5, 21.5}, {-16.0, 0.0}, b, 3.0, 0.0}};
    client::GenerationHistoryFrame frame;
    auto evaluate = [&] {
        auto history = client::GenerationMapHistory::prepare({SIDE, height, water, plates, 0.0}, cores);
        EXPECT_TRUE(history);
        return history && history->evaluate(.18, frame);
    };
    constexpr size_t OVERLAP = 22U * SIDE + 25U;
    ASSERT_TRUE(evaluate());
    EXPECT_DOUBLE_EQ(frame.elevation_m[OVERLAP], 150.0);
    EXPECT_DOUBLE_EQ(frame.water_level_m[OVERLAP], 30.0);
    EXPECT_FLOAT_EQ(frame.overlap_fraction[OVERLAP], 1.0F);
    for (size_t i = 0U; i < COUNT; ++i) if (a[i]) { height[i] += 20.0; water[i] += 10.0; }
    ASSERT_TRUE(evaluate());
    EXPECT_DOUBLE_EQ(frame.elevation_m[OVERLAP], 160.0);
    EXPECT_DOUBLE_EQ(frame.water_level_m[OVERLAP], 35.0);
    for (size_t i = 0U; i < COUNT; ++i) if (b[i]) { height[i] += 40.0; water[i] += 20.0; }
    ASSERT_TRUE(evaluate());
    EXPECT_DOUBLE_EQ(frame.elevation_m[OVERLAP], 180.0);
    EXPECT_DOUBLE_EQ(frame.water_level_m[OVERLAP], 45.0);
}
