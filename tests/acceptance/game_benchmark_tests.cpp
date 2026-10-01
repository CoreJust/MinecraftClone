#include <shared/world/World.hpp>

#include <acceptance/GameBenchmark.hpp>
#include <gtest/gtest.h>

#include <array>
#include <chrono>

using namespace std::chrono_literals;

TEST(GameBenchmark, DefaultsTo256AndRejectsUnsupportedRenderDistancesBeforeLaunch)
{
    static constexpr std::array<uint32_t, 3> INVALID_RADII{0U, 257U, 1'024U};
    EXPECT_EQ(acceptance::GameBenchmarkOptions{}.render_distance, 256U);
    for (uint32_t const radius : INVALID_RADII) {
        auto const result = acceptance::runGameBenchmark({.render_distance = radius});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), "render distance must be an integer chunk radius from 1 to 256");
    }
}

TEST(GameBenchmark, ClassifiesCompletedRequestsWithinHalfOpenIntervals)
{
    acceptance::GameBenchmarkOptions const options{};
    auto const first = std::chrono::steady_clock::time_point{};
    using acceptance::GameBenchmarkPhase;
    using acceptance::gameBenchmarkPhaseAt;

    EXPECT_EQ(gameBenchmarkPhaseAt(first - 1ns, first, options), GameBenchmarkPhase::Outside);
    EXPECT_EQ(gameBenchmarkPhaseAt(first, first, options), GameBenchmarkPhase::Cold);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 2s - 1ns, first, options), GameBenchmarkPhase::Cold);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 2s, first, options), GameBenchmarkPhase::Warm);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 5s - 1ns, first, options), GameBenchmarkPhase::Warm);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 5s, first, options), GameBenchmarkPhase::Uncapped);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 6s - 1ns, first, options), GameBenchmarkPhase::Uncapped);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 6s, first, options), GameBenchmarkPhase::Outside);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 6s + 1ns, first, options), GameBenchmarkPhase::Outside);
}

TEST(GameBenchmark, KeepsDiagnosticStationaryInputsMotionlessAcrossScriptBoundaries)
{
    using acceptance::GameBenchmarkWorkload;
    for (uint64_t const ordinal : { 0U, 19U, 20U, 39U, 40U, 100'000U }) {
        shared::Direction const direction = acceptance::gameBenchmarkDirection(
            GameBenchmarkWorkload::DiagnosticStationary, ordinal
        );
        EXPECT_EQ(direction.x, 0U);
        EXPECT_EQ(direction.y, 0U);
        EXPECT_EQ(direction.z, 0U);
        EXPECT_FALSE(direction.accelerated);
        EXPECT_FALSE(direction.cycle_movement_capabilities);
    }
    EXPECT_EQ(acceptance::gameBenchmarkWorkloadName(GameBenchmarkWorkload::DiagnosticStationary),
        "diagnostic-stationary-v1");
}

TEST(GameBenchmark, ScriptsDistinctMovementAndCapabilityWorkloads)
{
    using acceptance::GameBenchmarkWorkload;
    using acceptance::gameBenchmarkDirection;

    shared::Direction const ordinary = gameBenchmarkDirection(GameBenchmarkWorkload::OrdinaryMovement, 0U);
    shared::Direction const speed = gameBenchmarkDirection(GameBenchmarkWorkload::Speed200Movement, 0U);
    shared::Direction const border = gameBenchmarkDirection(GameBenchmarkWorkload::WrappedBorder, 0U);
    shared::Direction const churn = gameBenchmarkDirection(GameBenchmarkWorkload::PermissionCollisionChurn, 0U);

    EXPECT_EQ(ordinary.x, 1U);
    EXPECT_FALSE(ordinary.accelerated);
    EXPECT_EQ(speed.x, ordinary.x);
    EXPECT_TRUE(speed.accelerated);
    EXPECT_EQ(speed.speedup, 200U);
    EXPECT_EQ(border.speedup, 200U);
    EXPECT_EQ(gameBenchmarkDirection(GameBenchmarkWorkload::WrappedBorder, 25U).x, 127U);
    EXPECT_FALSE(churn.cycle_movement_capabilities);
    EXPECT_FALSE(gameBenchmarkDirection(GameBenchmarkWorkload::PermissionCollisionChurn, 1U)
        .cycle_movement_capabilities);
    EXPECT_EQ(acceptance::gameBenchmarkWorkloadName(GameBenchmarkWorkload::PermissionCollisionChurn),
        "permission-collision-churn-v1");
    EXPECT_EQ(static_cast<int8_t>(gameBenchmarkDirection(GameBenchmarkWorkload::OrdinaryMovement, 20U).x), -1);
}
