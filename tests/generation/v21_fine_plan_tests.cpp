#include "V21FinePlan.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <stop_token>

namespace
{
using namespace generation::detail;

double periodicDelta(double value)
{
    double wrapped = std::fmod(value + 32'768.0, 65'536.0);
    if (wrapped < 0.0)
    {
        wrapped += 65'536.0;
    }
    return wrapped - 32'768.0;
}

V21FineGeometry geometry()
{
    V21FineGeometry result;
    result.immutable_identity = "V21FinePlanTests/periodic-v-trough-v1";
    result.vertex_owner.resize(256U * 256U, 0);
    result.chords.push_back({.a = {1024.0, 1024.0}, .delta = {4.0, 0.0},
        .start = 0, .target = 1, .water_stage0 = 10.0, .water_stage1 = 9.96,
        .halfwidth = 1.5, .discharge_m3_s = .01});
    return result;
}

V21FineCallbacks callbacks(std::shared_ptr<size_t> calls = std::make_shared<size_t>(0U))
{
    V21FineCallbacks result;
    result.substrate = [calls](std::span<V21FinePoint const> points) {
        ++*calls;
        std::vector<double> output;
        for (auto const& p : points)
        {
            output.push_back(10.0 - .01 * periodicDelta(p[0] - 1024.0)
                + .65 * std::abs(periodicDelta(p[1] - 1024.0)));
        }
        return output;
    };
    result.base = result.substrate;
    result.standing = [](std::span<V21FinePoint const> points) {
        return std::vector<V21FineStanding>(points.size(), {0.0, 0});
    };
    result.protect = [](std::span<double const> values, std::span<V21FinePoint const>) {
        return std::vector<double>(values.begin(), values.end());
    };
    return result;
}

V21FineGrid const GRID{.origin_x_m = 1024.0, .origin_y_m = 1024.0,
    .spacing_m = 1.0, .rows = 5U, .columns = 5U};
} // namespace

TEST(V21FinePlanTests, BuildsWholeComponentAndCachesAcrossLods)
{
    auto input = geometry();
    auto distant = input.chords.front();
    distant.a = {30'000.0, 30'000.0};
    distant.start = 10;
    distant.target = 11;
    input.chords.push_back(distant);
    auto calls = std::make_shared<size_t>(0U);
    V21FineProvider provider{std::move(input), callbacks(calls)};
    auto plan = provider.forRegion(GRID, 3U);
    ASSERT_TRUE(plan) << plan.error().reason;
    ASSERT_EQ((*plan)->chords().size(), 2U);
    ASSERT_EQ((*plan)->connectedComponentIds().size(), 1U);
    EXPECT_EQ((*plan)->connectedComponentIds()[0U], "c00000000");
    EXPECT_EQ((*plan)->chords()[0U].global_order, 0U);
    EXPECT_EQ((*plan)->chords()[1U].global_order, 1U);
    EXPECT_EQ((*plan)->chords()[0U].water_stage1, (*plan)->chords()[1U].water_stage0);
    EXPECT_EQ((*plan)->chords()[0U].grade1, (*plan)->chords()[1U].grade0);
    size_t const before = *calls;
    auto coarse = provider.forRegion(GRID, 1U);
    ASSERT_TRUE(coarse);
    EXPECT_EQ(*calls, before);
}

TEST(V21FinePlanTests, ProfilesPeriodicCoreAndLeavesDistantTerrainUnchanged)
{
    V21FineProvider provider{geometry(), callbacks()};
    auto plan = provider.forRegion(GRID, 3U);
    ASSERT_TRUE(plan);
    std::vector<double> x{1025.0, 1025.0, 1025.0, 66'561.0};
    std::vector<double> y{1024.0, 1026.0, 2000.0, 1024.0};
    std::vector<V21FinePoint> points;
    for (size_t i = 0U; i < x.size(); ++i)
    {
        points.push_back({x[i], y[i]});
    }
    auto background = callbacks().substrate(points);
    auto profile = (*plan)->applyProfile(background, x, y, 3U);
    ASSERT_TRUE(profile);
    EXPECT_LT((*profile)[0U], background[0U]);
    EXPECT_LT((*profile)[1U], background[1U]);
    EXPECT_EQ((*profile)[2U], background[2U]);
    EXPECT_EQ((*profile)[0U], (*profile)[3U]);
    for (size_t i = 0U; i < x.size(); ++i)
    {
        EXPECT_LE((*profile)[i], background[i]);
    }
}

TEST(V21FinePlanTests, OwnershipAllowsForeignCoreButProtectsForeignBanks)
{
    auto input_callbacks = callbacks();
    input_callbacks.standing = [](std::span<V21FinePoint const> points) {
        return std::vector<V21FineStanding>(points.size(), {0.0, 1});
    };
    V21FineProvider provider{geometry(), std::move(input_callbacks)};
    auto plan = provider.forRegion(GRID, 3U);
    ASSERT_TRUE(plan);
    std::vector<double> x{1025.0, 1025.0}, y{1024.0, 1026.0};
    std::vector<double> background{9.99, 11.29};
    auto profile = (*plan)->applyProfile(background, x, y, 3U);
    ASSERT_TRUE(profile);
    EXPECT_LT((*profile)[0U], background[0U]);
    EXPECT_EQ((*profile)[1U], background[1U]);
}

TEST(V21FinePlanTests, ReportsTypedImmutableOutletFailureWithPhysicalWitness)
{
    auto input = geometry();
    input.chords[0U].shore_truncated = true;
    input.chords[0U].water_stage0 = 2.0;
    input.chords[0U].water_stage1 = 2.0;
    auto input_callbacks = callbacks();
    input_callbacks.substrate = [](std::span<V21FinePoint const> points) {
        return std::vector<double>(points.size(), 1.0);
    };
    input_callbacks.base = input_callbacks.substrate;
    input_callbacks.standing = [](std::span<V21FinePoint const> points) {
        std::vector<V21FineStanding> output;
        for (auto const& p : points)
        {
            output.push_back({2.0, p[0] >= 1027.5 ? 1 : 0});
        }
        return output;
    };
    V21FineProvider provider{std::move(input), std::move(input_callbacks)};
    auto result = provider.forRegion(GRID, 1U);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, V21FineFailureKind::ImmutableStandingOutletInfeasible);
    EXPECT_EQ(result.error().reason, "fine depression below immutable standing outlet");
    ASSERT_FALSE(result.error().witnesses.empty());
    auto const& witness = result.error().witnesses.front();
    EXPECT_EQ(witness.source_vertex, 0);
    EXPECT_DOUBLE_EQ(witness.required_stage, 2.0);
    EXPECT_DOUBLE_EQ(witness.maximum_cut_only_stage, .9998);
    EXPECT_DOUBLE_EQ(witness.deficit, 1.0002);
    EXPECT_DOUBLE_EQ(witness.deficit, witness.required_stage - witness.maximum_cut_only_stage);
    EXPECT_GT(witness.last_chord_m, 0.0);
}

TEST(V21FinePlanTests, LodZeroPreservesBackgroundWithoutStartingSubstrate)
{
    auto calls = std::make_shared<size_t>(0U);
    V21FineProvider provider{geometry(), callbacks(calls)};
    auto plan = provider.forRegion(GRID, 0U);
    ASSERT_TRUE(plan);
    EXPECT_TRUE((*plan)->chords().empty());
    EXPECT_EQ(*calls, 0U);
    std::vector<double> z{3.0}, x{1024.0}, y{1024.0};
    auto result = (*plan)->applyProfile(z, x, y, 0U);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, z);
}

TEST(V21FinePlanTests, LodZeroDoesNotApplyFineComponentCaps)
{
    auto input = geometry();
    input.chords.clear();
    for (int32_t index = 0; index < 257; ++index)
    {
        uint32_t const x_offset = static_cast<uint32_t>(index % 16) * 8U;
        uint32_t const y_offset = static_cast<uint32_t>(index / 16) * 8U;
        input.chords.push_back({
            .a = {1024.0 + x_offset, 1024.0 + y_offset},
            .delta = {4.0, 0.0},
            .start = index * 2,
            .target = index * 2 + 1,
            .water_stage0 = 10.0,
            .water_stage1 = 9.96,
            .halfwidth = 1.5,
            .discharge_m3_s = .01,
        });
    }
    auto calls = std::make_shared<size_t>(0U);
    auto native_callbacks = callbacks(calls);
    auto const standing = native_callbacks.standing;
    auto const protect = native_callbacks.protect;
    native_callbacks.standing = [calls, standing](std::span<V21FinePoint const> points) {
        ++*calls;
        return standing(points);
    };
    native_callbacks.protect = [calls, protect](std::span<double const> z,
        std::span<V21FinePoint const> points) {
        ++*calls;
        return protect(z, points);
    };
    V21FineProvider provider{std::move(input), std::move(native_callbacks)};
    V21FineGrid const broad_grid{
        .origin_x_m = 1024.0,
        .origin_y_m = 1024.0,
        .spacing_m = 1.0,
        .rows = 257U,
        .columns = 257U,
    };
    auto plan = provider.forRegion(broad_grid, 0U);
    ASSERT_TRUE(plan) << plan.error().reason;
    EXPECT_TRUE((*plan)->chords().empty());
    EXPECT_TRUE((*plan)->connectedComponentIds().empty());
    EXPECT_EQ(*calls, 0U);
    auto fine_plan = provider.forRegion(broad_grid, 1U);
    ASSERT_FALSE(fine_plan);
    EXPECT_EQ(fine_plan.error().kind, V21FineFailureKind::LimitExceeded);
    EXPECT_EQ(fine_plan.error().reason, "region exceeds whole-component fine plan caps");
    EXPECT_EQ(*calls, 0U);
}

TEST(V21FinePlanTests, CancellationAndQueryLimitHaveDistinctTypedFailures)
{
    V21FineProvider provider{geometry(), callbacks()};
    std::stop_source cancellation;
    cancellation.request_stop();
    auto cancelled = provider.forRegion(GRID, 3U, cancellation.get_token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().kind, V21FineFailureKind::Cancelled);
    auto oversized = GRID;
    oversized.rows = 1025U;
    oversized.columns = 1024U;
    auto limited = provider.forRegion(oversized, 3U);
    ASSERT_FALSE(limited);
    EXPECT_EQ(limited.error().kind, V21FineFailureKind::LimitExceeded);
}

TEST(V21FinePlanTests, RejectsMismatchedNativeCallbackOutput)
{
    auto input_callbacks = callbacks();
    input_callbacks.substrate = [](std::span<V21FinePoint const>) { return std::vector<double>{}; };
    V21FineProvider provider{geometry(), std::move(input_callbacks)};
    auto result = provider.forRegion(GRID, 3U);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, V21FineFailureKind::InvalidInput);
}

TEST(V21FinePlanTests, ContractsRoundedShoreSelfLoopAndTransfersStandingStage)
{
    V21FinePoint const contact{43'775.999'999'046'326, 38'143.999'999'046'326};
    constexpr double STAGE0 = 9.259'948'302'262'913;
    constexpr double CONTACT_STAGE = 9.259'834'865'507'74;
    constexpr double NATIVE_Q = 0.123'456'789;
    auto input = geometry();
    input.immutable_identity = "V21FinePlanTests/rounded-shore-contact-v1";
    input.chords.clear();
    input.chords.push_back({
        .a = {contact[0] - 1.0, contact[1]},
        .delta = {1.0, 0.0},
        .start = 38'315,
        .target = 38'058,
        .water_stage0 = STAGE0 + 0.0002,
        .water_stage1 = STAGE0,
        .halfwidth = 3.194'939'795'805'2748,
        .discharge_m3_s = NATIVE_Q,
        .global_order = 0U,
    });
    input.chords.push_back({
        .a = contact,
        .delta = {-6.749'178'282'916'546e-08, -6.749'178'282'916'546e-08},
        .start = 38'315,
        .target = 38'058,
        .water_stage0 = STAGE0,
        .water_stage1 = CONTACT_STAGE,
        .halfwidth = 3.194'939'795'805'2748,
        .discharge_m3_s = NATIVE_Q,
        .shore_truncated = true,
        .global_order = 1U,
    });
    auto input_callbacks = callbacks();
    input_callbacks.substrate = [](std::span<V21FinePoint const> points) {
        return std::vector<double>(points.size(), CONTACT_STAGE + 1.0);
    };
    input_callbacks.base = input_callbacks.substrate;
    V21FineProvider provider{std::move(input), std::move(input_callbacks)};
    V21FineGrid const contact_grid{
        .origin_x_m = contact[0], .origin_y_m = contact[1], .spacing_m = 1.0,
        .rows = 5U, .columns = 5U,
    };

    auto result = provider.forRegion(contact_grid, 1U);

    ASSERT_TRUE(result) << result.error().reason;
    ASSERT_EQ((*result)->chords().size(), 1U);
    EXPECT_EQ((*result)->chords()[0U].global_order, 0U);
    EXPECT_DOUBLE_EQ((*result)->chords()[0U].discharge_m3_s, NATIVE_Q);
    EXPECT_DOUBLE_EQ((*result)->chords()[0U].water_stage1, CONTACT_STAGE);
}

TEST(V21FinePlanTests, RejectsRoundedNonShoreSelfLoop)
{
    V21FinePoint const contact{43'775.999'999'046'326, 38'143.999'999'046'326};
    auto input = geometry();
    input.chords.clear();
    input.chords.push_back({
        .a = contact,
        .delta = {-6.749'178'282'916'546e-08, -6.749'178'282'916'546e-08},
        .start = 38'315,
        .target = 38'058,
        .water_stage0 = 9.259'948'302'262'913,
        .water_stage1 = 9.259'834'865'507'74,
        .halfwidth = 3.194'939'795'805'2748,
        .discharge_m3_s = 0.123'456'789,
    });
    auto input_callbacks = callbacks();
    input_callbacks.substrate = [](std::span<V21FinePoint const> points) {
        return std::vector<double>(points.size(), 10.5);
    };
    input_callbacks.base = input_callbacks.substrate;
    V21FineProvider provider{std::move(input), std::move(input_callbacks)};
    V21FineGrid const contact_grid{
        .origin_x_m = contact[0], .origin_y_m = contact[1], .spacing_m = 1.0,
        .rows = 5U, .columns = 5U,
    };

    auto result = provider.forRegion(contact_grid, 1U);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, V21FineFailureKind::CyclicGraph);
}

TEST(V21FinePlanTests, RejectsTwoEdgeNonShoreCycle)
{
    auto input = geometry();
    input.chords.clear();
    input.chords.push_back({
        .a = {1024.0, 1024.0}, .delta = {2.0, 0.0},
        .start = 10, .target = 11, .water_stage0 = 10.0, .water_stage1 = 9.9,
        .halfwidth = 1.5, .discharge_m3_s = .01, .global_order = 0U,
    });
    input.chords.push_back({
        .a = {1026.0, 1024.0}, .delta = {-2.0, 0.0},
        .start = 11, .target = 10, .water_stage0 = 9.9, .water_stage1 = 10.0,
        .halfwidth = 1.5, .discharge_m3_s = .01, .global_order = 1U,
    });
    auto input_callbacks = callbacks();
    input_callbacks.substrate = [](std::span<V21FinePoint const> points) {
        return std::vector<double>(points.size(), 11.0);
    };
    input_callbacks.base = input_callbacks.substrate;
    V21FineProvider provider{std::move(input), std::move(input_callbacks)};

    auto result = provider.forRegion(GRID, 1U);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, V21FineFailureKind::CyclicGraph);
}

TEST(V21FinePlanTests, RejectsConflictingRoundedShoreOutletStages)
{
    V21FinePoint const contact{1024.0, 1024.0};
    auto input = geometry();
    input.chords.clear();
    for (uint64_t order = 0U; order < 2U; ++order)
    {
        input.chords.push_back({
            .a = contact,
            .delta = {order == 0U ? 1e-8 : -1e-8, 0.0},
            .start = 10,
            .target = 11,
            .water_stage0 = 10.0,
            .water_stage1 = order == 0U ? 9.0 : 9.1,
            .halfwidth = 1.5,
            .discharge_m3_s = .01,
            .shore_truncated = true,
            .global_order = order,
        });
    }
    auto input_callbacks = callbacks();
    input_callbacks.substrate = [](std::span<V21FinePoint const> points) {
        return std::vector<double>(points.size(), 10.5);
    };
    input_callbacks.base = input_callbacks.substrate;
    V21FineProvider provider{std::move(input), std::move(input_callbacks)};
    V21FineGrid const contact_grid{
        .origin_x_m = contact[0], .origin_y_m = contact[1], .spacing_m = 1.0,
        .rows = 5U, .columns = 5U,
    };

    auto result = provider.forRegion(contact_grid, 1U);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, V21FineFailureKind::ConflictingStandingOutlet);
}

TEST(V21FinePlanTests, RegionCandidatesPreservePeriodicAndBinBoundarySelection)
{
    auto input = geometry();
    input.chords.clear();
    for (double x : {65'534.0, 256.0, 32'768.0})
    {
        auto chord = geometry().chords.front();
        chord.a[0] = x;
        input.chords.push_back(chord);
    }
    auto input_callbacks = callbacks();
    input_callbacks.substrate = [](std::span<V21FinePoint const> points) {
        return std::vector<double>(points.size(), 10.0);
    };
    input_callbacks.base = input_callbacks.substrate;
    V21FineProvider provider{std::move(input), std::move(input_callbacks)};
    // The wrapped interval crosses the zero and 256m bin boundaries. Its
    // conservative support selects both nearby whole components, in source order.
    V21FineGrid const wrapped{
        .origin_x_m = 65'534.0, .origin_y_m = 1024.0, .spacing_m = 128.0,
        .rows = 1U, .columns = 5U,
    };
    auto plan = provider.forRegion(wrapped, 3U);
    ASSERT_TRUE(plan) << plan.error().reason;
    ASSERT_EQ((*plan)->connectedComponentIds().size(), 2U);
    EXPECT_EQ((*plan)->connectedComponentIds()[0U], "c00000000");
    EXPECT_EQ((*plan)->connectedComponentIds()[1U], "c00000001");
    ASSERT_EQ((*plan)->chords().size(), 4U);

    auto outside = wrapped;
    outside.origin_x_m = 1024.0;
    outside.columns = 1U;
    auto empty = provider.forRegion(outside, 3U);
    ASSERT_TRUE(empty);
    EXPECT_TRUE((*empty)->connectedComponentIds().empty());

    auto far = outside;
    far.origin_x_m = 32'768.0;
    auto distant = provider.forRegion(far, 3U);
    ASSERT_TRUE(distant);
    ASSERT_EQ((*distant)->connectedComponentIds().size(), 1U);
    EXPECT_EQ((*distant)->connectedComponentIds()[0U], "c00000002");
}
