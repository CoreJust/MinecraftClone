#include <shared/world/World.hpp>

#include <acceptance/GameBenchmark.hpp>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>

using namespace std::chrono_literals;

namespace {

acceptance::GameBenchmarkTerrainObservation oneNearTile(
    shared::HeightTileKey const center,
    shared::HeightTileKey const key,
    bool const resident,
    bool const current_mesh_visible,
    uint64_t const accepted_near_admissions,
    uint64_t const accepted_far_admissions = 0U,
    uint64_t const far_admissions_while_near_pending = 0U
)
{
    acceptance::GameBenchmarkTerrainObservation observation{};
    observation.center = center;
    observation.near_tiles[112U] = acceptance::GameBenchmarkTerrainTileObservation{
        .key = key,
        .desired = true,
        .resident = resident,
        .current_mesh_visible = current_mesh_visible,
        .pending = resident && !current_mesh_visible,
    };
    observation.desired_count = 1U;
    observation.resident_count = resident ? 1U : 0U;
    observation.nonresident_count = resident ? 0U : 1U;
    observation.current_mesh_visible_count = current_mesh_visible ? 1U : 0U;
    observation.pending_count = resident && !current_mesh_visible ? 1U : 0U;
    observation.accepted_near_admissions = accepted_near_admissions;
    observation.accepted_far_admissions = accepted_far_admissions;
    observation.far_admissions_while_near_pending = far_admissions_while_near_pending;
    return observation;
}

} // namespace

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
    EXPECT_EQ(acceptance::gameBenchmarkWorkloadName(GameBenchmarkWorkload::Speed200Movement),
        "speed-200-movement-v2");
    EXPECT_EQ(speed.x, 127U);
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
    for (uint64_t const ordinal : { 0U, 19U, 20U, 39U, 40U }) {
        shared::Direction const travel = gameBenchmarkDirection(GameBenchmarkWorkload::Speed200Movement, ordinal);
        EXPECT_EQ(travel.x, 127U);
        EXPECT_TRUE(travel.accelerated);
        EXPECT_EQ(travel.speedup, 200U);
    }
}

TEST(GameBenchmark, Speed200MovementUsesFullWrappedFlightDisplacementPerTick)
{
    static constexpr int32_t START_X = shared::World::FLIGHT_MAX_CELL - 50;
    static constexpr int32_t START_Y = 100;
    static constexpr int32_t START_Z = 800;
    static constexpr uint64_t WORLD_EXTENT_SUBCELLS =
        (static_cast<uint64_t>(shared::World::FLIGHT_MAX_CELL) + 1U) * shared::SUBCELLS_PER_CELL;
    static constexpr uint64_t EXPECTED_DELTA_SUBCELLS =
        static_cast<uint64_t>(shared::MOVEMENT_SUBCELLS_PER_TICK) * 200U;

    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = START_X, .y = START_Y, .z = START_Z });
    std::optional<shared::Player> const before = world.player(1U);
    ASSERT_TRUE(before.has_value());

    shared::Direction const direction = acceptance::gameBenchmarkDirection(
        acceptance::GameBenchmarkWorkload::Speed200Movement, 0U
    );
    ASSERT_EQ(direction.x, 127U);
    ASSERT_TRUE(direction.accelerated);
    ASSERT_EQ(direction.speedup, 200U);
    ASSERT_TRUE(world.movePlayer(1U, direction, shared::TICK));

    std::optional<shared::Player> const after = world.player(1U);
    ASSERT_TRUE(after.has_value());
    auto const horizontalSubcells = [](shared::Player const& player) {
        return static_cast<uint64_t>(player.x) * shared::SUBCELLS_PER_CELL + player.x_subcell;
    };
    uint64_t const expected_position =
        (horizontalSubcells(before.value()) + EXPECTED_DELTA_SUBCELLS) % WORLD_EXTENT_SUBCELLS;
    EXPECT_EQ(horizontalSubcells(after.value()), expected_position);
    EXPECT_LT(after.value().x, before.value().x);
}

TEST(GameBenchmark, CompletesResidentToVisibleReadinessAcrossMovingCenters)
{
    using Clock = std::chrono::steady_clock;

    acceptance::GameBenchmarkTerrainReadinessTracker tracker;
    Clock::time_point const start{};
    tracker.observe(oneNearTile({ 0, 0 }, { 0, 0 }, false, false, 0U), start);
    EXPECT_EQ(tracker.evidence().eligible_mesh_episode_count, 0U);
    tracker.observe(oneNearTile({ 1, 0 }, { 0, 0 }, true, false, 1U), start + 1s);
    tracker.observe(oneNearTile({ 2, 0 }, { 0, 0 }, true, true, 1U), start + 4s);
    tracker.observe(oneNearTile({ 2, 0 }, { 0, 0 }, true, true, 1U), start + 4s + 100ms);

    acceptance::GameBenchmarkTerrainReadinessEvidence const evidence = tracker.evidence();
    EXPECT_EQ(evidence.center_change_count, 2U);
    EXPECT_EQ(evidence.eligible_mesh_episode_count, 1U);
    EXPECT_EQ(evidence.ready_mesh_episode_count, 1U);
    EXPECT_EQ(evidence.deadline_miss_count, 0U);
    EXPECT_EQ(evidence.open_mesh_episode_count, 0U);
    EXPECT_EQ(evidence.max_ready_latency_ns, 3'000'000'000U);
    EXPECT_EQ(evidence.far_admissions_while_near_pending, 0U);
    EXPECT_EQ(evidence.final_desired_tile_count, 1U);
    EXPECT_EQ(evidence.final_current_mesh_visible_count, 1U);
    EXPECT_TRUE(evidence.complete);
}

TEST(GameBenchmark, RejectsFarAdmissionWhileCurrentNearWorkIsPending)
{
    using Clock = std::chrono::steady_clock;

    acceptance::GameBenchmarkTerrainReadinessTracker tracker;
    Clock::time_point const start{};
    tracker.observe(oneNearTile(
        { 0, 0 }, { 0, 0 }, true, false, 1U, 1U, 1U
    ), start);
    tracker.observe(oneNearTile(
        { 1, 0 }, { 0, 0 }, true, true, 1U, 1U, 1U
    ), start + 1s);
    tracker.observe(oneNearTile(
        { 2, 0 }, { 0, 0 }, true, true, 1U, 1U, 1U
    ), start + 1100ms);

    acceptance::GameBenchmarkTerrainReadinessEvidence const evidence = tracker.evidence();
    EXPECT_EQ(evidence.center_change_count, 2U);
    EXPECT_EQ(evidence.eligible_mesh_episode_count, 1U);
    EXPECT_EQ(evidence.ready_mesh_episode_count, 1U);
    EXPECT_EQ(evidence.final_desired_tile_count, 1U);
    EXPECT_EQ(evidence.final_current_mesh_visible_count, 1U);
    EXPECT_EQ(evidence.accepted_near_admissions, 1U);
    EXPECT_EQ(evidence.far_admissions_while_near_pending, 1U);
    EXPECT_FALSE(evidence.complete);
}

TEST(GameBenchmark, RecordsDeadlineMissEvenWhenMeshBecomesVisibleLater)
{
    using Clock = std::chrono::steady_clock;

    acceptance::GameBenchmarkTerrainReadinessTracker tracker;
    Clock::time_point const start{};
    tracker.observe(oneNearTile({ 0, 0 }, { 0, 0 }, true, false, 1U), start);
    tracker.observe(oneNearTile({ 1, 0 }, { 0, 0 }, true, false, 1U), start + 4s + 1ms);
    tracker.observe(oneNearTile({ 2, 0 }, { 0, 0 }, true, true, 1U), start + 5s);

    acceptance::GameBenchmarkTerrainReadinessEvidence const evidence = tracker.evidence();
    EXPECT_EQ(evidence.ready_mesh_episode_count, 1U);
    EXPECT_EQ(evidence.deadline_miss_count, 1U);
    EXPECT_EQ(evidence.open_mesh_episode_count, 0U);
    EXPECT_FALSE(evidence.complete);
}

TEST(GameBenchmark, DoesNotCompleteWhenCurrentNearIncludesNonresidentTiles)
{
    using Clock = std::chrono::steady_clock;

    acceptance::GameBenchmarkTerrainReadinessTracker tracker;
    Clock::time_point const start{};
    tracker.observe(oneNearTile({ 0, 0 }, { 0, 0 }, true, false, 1U), start);
    tracker.observe(oneNearTile({ 1, 0 }, { 0, 0 }, true, true, 1U), start + 1s);

    acceptance::GameBenchmarkTerrainObservation final_sample{};
    final_sample.center = shared::HeightTileKey{ 2, 0 };
    final_sample.near_tiles[112U] = acceptance::GameBenchmarkTerrainTileObservation{
        .key = { 1, 0 },
        .desired = true,
        .resident = true,
        .current_mesh_visible = true,
    };
    final_sample.near_tiles[113U] = acceptance::GameBenchmarkTerrainTileObservation{
        .key = { 2, 0 },
        .desired = true,
    };
    final_sample.desired_count = 2U;
    final_sample.resident_count = 1U;
    final_sample.nonresident_count = 1U;
    final_sample.current_mesh_visible_count = 1U;
    final_sample.accepted_near_admissions = 1U;
    tracker.observe(final_sample, start + 2s);

    acceptance::GameBenchmarkTerrainReadinessEvidence const evidence = tracker.evidence();
    EXPECT_EQ(evidence.eligible_mesh_episode_count, 1U);
    EXPECT_EQ(evidence.ready_mesh_episode_count, 1U);
    EXPECT_EQ(evidence.final_desired_tile_count, 2U);
    EXPECT_EQ(evidence.final_current_mesh_visible_count, 1U);
    EXPECT_EQ(evidence.final_nonresident_tile_count, 1U);
    EXPECT_FALSE(evidence.complete);
}

TEST(GameBenchmark, CensorsDepartedKeyAndKeepsCurrentMissingEpisodeOpen)
{
    using Clock = std::chrono::steady_clock;

    acceptance::GameBenchmarkTerrainReadinessTracker tracker;
    Clock::time_point const start{};
    acceptance::GameBenchmarkTerrainObservation first = oneNearTile(
        { 0, 0 }, { 0, 0 }, true, false, 1U
    );
    tracker.observe(first, start);

    acceptance::GameBenchmarkTerrainObservation second = oneNearTile(
        { 1, 0 }, { 0, 0 }, true, false, 2U
    );
    second.near_tiles[113U] = acceptance::GameBenchmarkTerrainTileObservation{
        .key = { 1, 0 },
        .desired = true,
        .resident = true,
        .pending = true,
    };
    ++second.desired_count;
    ++second.resident_count;
    ++second.pending_count;
    tracker.observe(second, start + 1s);

    acceptance::GameBenchmarkTerrainObservation final_sample{};
    final_sample.center = shared::HeightTileKey{ 2, 0 };
    final_sample.near_tiles[112U] = acceptance::GameBenchmarkTerrainTileObservation{
        .key = { 1, 0 },
        .desired = true,
        .resident = true,
        .pending = true,
    };
    final_sample.desired_count = 1U;
    final_sample.resident_count = 1U;
    final_sample.pending_count = 1U;
    final_sample.accepted_near_admissions = 2U;
    tracker.observe(final_sample, start + 1500ms);

    acceptance::GameBenchmarkTerrainReadinessEvidence const evidence = tracker.evidence();
    EXPECT_EQ(evidence.eligible_mesh_episode_count, 2U);
    EXPECT_EQ(evidence.ready_mesh_episode_count, 0U);
    EXPECT_EQ(evidence.left_near_unready_episode_count, 1U);
    EXPECT_EQ(evidence.open_mesh_episode_count, 1U);
    EXPECT_EQ(evidence.deadline_miss_count, 0U);
    EXPECT_FALSE(evidence.complete);
}
