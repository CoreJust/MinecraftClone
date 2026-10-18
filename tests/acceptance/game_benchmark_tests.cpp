#include <shared/net/Message.hpp>
#include <shared/world/World.hpp>

#include <acceptance/EvidenceJson.hpp>
#include <acceptance/GameBenchmark.hpp>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;

namespace {

struct OneNearTileFixture final {
    shared::HeightTileKey center{};
    shared::HeightTileKey key{};
    bool resident = false;
    bool current_mesh_visible = false;
    uint64_t accepted_near_admissions = 0U;
    uint64_t accepted_far_admissions = 0U;
    uint64_t far_admissions_while_near_pending = 0U;
};

acceptance::GameBenchmarkTerrainObservation oneNearTile(OneNearTileFixture const fixture)
{
    acceptance::GameBenchmarkTerrainObservation observation{};
    observation.center = fixture.center;
    observation.near_tiles[112U] = acceptance::GameBenchmarkTerrainTileObservation{
        .key = fixture.key,
        .desired = true,
        .resident = fixture.resident,
        .current_mesh_visible = fixture.current_mesh_visible,
        .pending = fixture.resident && !fixture.current_mesh_visible,
    };
    observation.desired_count = 1U;
    observation.resident_count = fixture.resident ? 1U : 0U;
    observation.nonresident_count = fixture.resident ? 0U : 1U;
    observation.current_mesh_visible_count = fixture.current_mesh_visible ? 1U : 0U;
    observation.pending_count = fixture.resident && !fixture.current_mesh_visible ? 1U : 0U;
    observation.accepted_near_admissions = fixture.accepted_near_admissions;
    observation.accepted_far_admissions = fixture.accepted_far_admissions;
    observation.far_admissions_while_near_pending = fixture.far_admissions_while_near_pending;
    return observation;
}

acceptance::GameBenchmarkTerrainObservation fullNearWindow(
    shared::HeightTileKey const center,
    uint32_t const current_mesh_visible_count
)
{
    acceptance::GameBenchmarkTerrainObservation observation{};
    observation.center = center;
    observation.desired_count = acceptance::GAME_BENCHMARK_TERRAIN_NEAR_TILE_COUNT;
    observation.current_mesh_visible_count = current_mesh_visible_count;
    observation.resident_count = current_mesh_visible_count;
    observation.nonresident_count = acceptance::GAME_BENCHMARK_TERRAIN_NEAR_TILE_COUNT
        - current_mesh_visible_count;
    uint32_t index = 0U;
    for (int32_t y = -7; y <= 7; ++y) {
        for (int32_t x = -7; x <= 7; ++x) {
            bool const visible = index < current_mesh_visible_count;
            observation.near_tiles[index] = acceptance::GameBenchmarkTerrainTileObservation{
                .key = { center.x + x, center.y + y },
                .desired = true,
                .resident = visible,
                .current_mesh_visible = visible,
            };
            ++index;
        }
    }
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

TEST(GameBenchmark, BoundsFullRadiusWarmupTimeoutAndWatchdogBudget)
{
    EXPECT_EQ(*acceptance::gameBenchmarkFullRadiusWarmupTimeout(1), 1s);
    EXPECT_EQ(*acceptance::gameBenchmarkFullRadiusWarmupTimeout(180), 180s);
    EXPECT_EQ(*acceptance::gameBenchmarkFullRadiusWarmupTimeout(1'800), 1'800s);
    for (int64_t const seconds : { -1, 0, 1'801 }) {
        EXPECT_FALSE(acceptance::gameBenchmarkFullRadiusWarmupTimeout(seconds));
    }

    acceptance::GameBenchmarkOptions options{
        .workload = acceptance::GameBenchmarkWorkload::Speed200FullRadiusRecovery,
    };
    EXPECT_EQ(acceptance::gameBenchmarkDeadline(options), 416s);
    options.workload = acceptance::GameBenchmarkWorkload::Speed200Movement;
    EXPECT_EQ(acceptance::gameBenchmarkDeadline(options), options.deadline);
}

TEST(GameBenchmark, SuccessfulRequestIntervalsIncludeFailedAttemptsAndStayWithinTheirStationaryPhase)
{
    static constexpr uint64_t MILLISECOND_NS = 1'000'000U;
    std::array<acceptance::GameBenchmarkFrameSample, 6U> const frames{
        acceptance::GameBenchmarkFrameSample{
            .presentation_succeeded = true,
            .started_offset_ns = 0U,
            .loop_duration_ns = 2U * MILLISECOND_NS,
            .terrain_phase = "baseline-stationary",
        },
        acceptance::GameBenchmarkFrameSample{
            .presentation_succeeded = false,
            .started_offset_ns = 2U * MILLISECOND_NS,
            .loop_duration_ns = 8U * MILLISECOND_NS,
            .terrain_phase = "baseline-stationary",
        },
        acceptance::GameBenchmarkFrameSample{
            .presentation_succeeded = true,
            .started_offset_ns = 10U * MILLISECOND_NS,
            .loop_duration_ns = 2U * MILLISECOND_NS,
            .terrain_phase = "baseline-stationary",
        },
        acceptance::GameBenchmarkFrameSample{
            .presentation_succeeded = true,
            .started_offset_ns = 100U * MILLISECOND_NS,
            .loop_duration_ns = 2U * MILLISECOND_NS,
            .terrain_phase = "speed-200-movement",
        },
        acceptance::GameBenchmarkFrameSample{
            .presentation_succeeded = true,
            .started_offset_ns = 1'000U * MILLISECOND_NS,
            .loop_duration_ns = 2U * MILLISECOND_NS,
            .terrain_phase = "recovered-stationary",
        },
        acceptance::GameBenchmarkFrameSample{
            .presentation_succeeded = true,
            .started_offset_ns = 1'020U * MILLISECOND_NS,
            .loop_duration_ns = 2U * MILLISECOND_NS,
            .terrain_phase = "recovered-stationary",
        },
    };

    auto const baseline = acceptance::gameBenchmarkSuccessfulPresentIntervals(frames, "baseline-stationary");
    ASSERT_EQ(baseline.size(), 1U);
    EXPECT_EQ(baseline.front(), 10ms);
    auto const recovered = acceptance::gameBenchmarkSuccessfulPresentIntervals(frames, "recovered-stationary");
    ASSERT_EQ(recovered.size(), 1U);
    EXPECT_EQ(recovered.front(), 20ms);
    EXPECT_TRUE(acceptance::gameBenchmarkSuccessfulPresentIntervals(frames, "speed-200-movement").empty());
}

TEST(GameBenchmark, RejectsFullRadiusWorkloadWithoutFullInterestBeforeRuntimeStarts)
{
    acceptance::GameBenchmarkOptions const options{
        .workload = acceptance::GameBenchmarkWorkload::Speed200FullRadiusRecovery,
        .render_distance = 255U,
    };
    auto const result = acceptance::runGameBenchmark(options);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), "speed-200 full-radius recovery requires render distance 256");
}

TEST(GameBenchmark, FullRadiusSequenceRequiresBothExactBarriersAndStationarySamples)
{
    using Clock = std::chrono::steady_clock;
    using acceptance::GameBenchmarkFullRadiusPhase;
    using acceptance::GameBenchmarkFullRadiusSequence;

    Clock::time_point const start{};
    GameBenchmarkFullRadiusSequence sequence{ 30s };
    sequence.start(start);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::InitialWarmup);
    sequence.observe(start + 10s, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::InitialWarmup);
    sequence.observe(start + 12s, false);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::InitialWarmup);
    sequence.observe(start + 15s, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::BaselineStationary);
    sequence.observe(start + 25s - 1ns, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::BaselineStationary);
    sequence.observe(start + 25s, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Movement);
    sequence.observe(start + 31s - 1ns, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Movement);
    sequence.observe(start + 31s, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveryWarmup);
    sequence.observe(start + 60s, false);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveryWarmup);
    sequence.observe(start + 61s, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveredStationary);
    sequence.observe(start + 71s - 1ns, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveredStationary);
    sequence.observe(start + 71s, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Complete);
}

TEST(GameBenchmark, FullRadiusWarmupTimeoutsAreIndependentAndFailureIsTerminal)
{
    using Clock = std::chrono::steady_clock;
    using acceptance::GameBenchmarkFullRadiusPhase;
    using acceptance::GameBenchmarkFullRadiusSequence;

    Clock::time_point const start{};
    GameBenchmarkFullRadiusSequence initial_timeout{ 2s };
    initial_timeout.start(start);
    initial_timeout.observe(start + 2s, std::nullopt);
    EXPECT_EQ(initial_timeout.phase(), GameBenchmarkFullRadiusPhase::Failed);
    initial_timeout.observe(start + 3s, true);
    EXPECT_EQ(initial_timeout.phase(), GameBenchmarkFullRadiusPhase::Failed);

    GameBenchmarkFullRadiusSequence recovery_timeout{ 2s };
    recovery_timeout.start(start);
    recovery_timeout.observe(start, true);
    recovery_timeout.observe(start + 10s, std::nullopt);
    recovery_timeout.observe(start + 16s, std::nullopt);
    ASSERT_EQ(recovery_timeout.phase(), GameBenchmarkFullRadiusPhase::RecoveryWarmup);
    recovery_timeout.observe(start + 18s, std::nullopt);
    EXPECT_EQ(recovery_timeout.phase(), GameBenchmarkFullRadiusPhase::Failed);
}

TEST(GameBenchmark, FullRadiusStationaryEpochsStartAfterDelayedBarrierScans)
{
    using Clock = std::chrono::steady_clock;
    using acceptance::GameBenchmarkFullRadiusPhase;
    static constexpr Clock::time_point START{};
    static constexpr std::chrono::seconds WARMUP_TIMEOUT{30};
    static constexpr std::chrono::seconds INITIAL_FRAME_FINISHED{4};
    static constexpr std::chrono::seconds SCAN_DELAY{1};

    acceptance::GameBenchmarkFullRadiusSequence sequence{WARMUP_TIMEOUT};
    sequence.start(START);
    Clock::time_point const initial_scan_finished = START + INITIAL_FRAME_FINISHED + SCAN_DELAY;
    sequence.observe(initial_scan_finished, true);
    ASSERT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::BaselineStationary);
    EXPECT_EQ(sequence.phaseElapsed(initial_scan_finished), Clock::duration::zero());
    Clock::time_point const movement_started = initial_scan_finished
        + acceptance::GAME_BENCHMARK_FULL_RADIUS_STATIONARY_SAMPLE;
    sequence.observe(movement_started - SCAN_DELAY, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::BaselineStationary);
    sequence.observe(movement_started, std::nullopt);
    ASSERT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Movement);
    Clock::time_point const recovery_started = movement_started + acceptance::GAME_BENCHMARK_FULL_RADIUS_MOVEMENT;
    sequence.observe(recovery_started, std::nullopt);
    ASSERT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveryWarmup);
    Clock::time_point const recovery_scan_finished = recovery_started + SCAN_DELAY;
    sequence.observe(recovery_scan_finished, true);
    ASSERT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveredStationary);
    EXPECT_EQ(sequence.phaseElapsed(recovery_scan_finished), Clock::duration::zero());
    Clock::time_point const stationary_finished = recovery_scan_finished
        + acceptance::GAME_BENCHMARK_FULL_RADIUS_STATIONARY_SAMPLE;
    sequence.observe(stationary_finished - SCAN_DELAY, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveredStationary);
    sequence.observe(stationary_finished, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Complete);
}

TEST(GameBenchmark, FullRadiusInitialExactBarrierFinishingAfterDeadlineFails)
{
    using Clock = std::chrono::steady_clock;
    static constexpr Clock::time_point START{};
    static constexpr std::chrono::seconds WARMUP_TIMEOUT{30};

    acceptance::GameBenchmarkFullRadiusSequence sequence{WARMUP_TIMEOUT};
    sequence.start(START);
    sequence.observe(START + WARMUP_TIMEOUT + 1ns, true);
    EXPECT_EQ(sequence.phase(), acceptance::GameBenchmarkFullRadiusPhase::Failed);
}

TEST(GameBenchmark, FullRadiusRecoveryExactBarrierFinishingAfterDeadlineFails)
{
    using Clock = std::chrono::steady_clock;
    using acceptance::GameBenchmarkFullRadiusPhase;
    static constexpr Clock::time_point START{};
    static constexpr std::chrono::seconds WARMUP_TIMEOUT{30};

    acceptance::GameBenchmarkFullRadiusSequence sequence{WARMUP_TIMEOUT};
    sequence.start(START);
    sequence.observe(START, true);
    Clock::time_point const movement_started = START + acceptance::GAME_BENCHMARK_FULL_RADIUS_STATIONARY_SAMPLE;
    sequence.observe(movement_started, std::nullopt);
    Clock::time_point const recovery_started = movement_started + acceptance::GAME_BENCHMARK_FULL_RADIUS_MOVEMENT;
    sequence.observe(recovery_started, std::nullopt);
    ASSERT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveryWarmup);
    sequence.observe(recovery_started + WARMUP_TIMEOUT + 1ns, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Failed);
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
    tracker.observe(oneNearTile({
        .center = { 0, 0 },
        .key = { 0, 0 },
        .resident = false,
        .current_mesh_visible = false,
        .accepted_near_admissions = 0U,
    }), start);
    EXPECT_EQ(tracker.evidence().eligible_mesh_episode_count, 0U);
    tracker.observe(oneNearTile({
        .center = { 1, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = false,
        .accepted_near_admissions = 1U,
    }), start + 1s);
    tracker.observe(oneNearTile({
        .center = { 2, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = true,
        .accepted_near_admissions = 1U,
    }), start + 4s);
    tracker.observe(oneNearTile({
        .center = { 2, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = true,
        .accepted_near_admissions = 1U,
    }), start + 4s + 100ms);

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
    tracker.observe(oneNearTile({
        .center = { 0, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = false,
        .accepted_near_admissions = 1U,
        .accepted_far_admissions = 1U,
        .far_admissions_while_near_pending = 1U,
    }), start);
    tracker.observe(oneNearTile({
        .center = { 1, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = true,
        .accepted_near_admissions = 1U,
        .accepted_far_admissions = 1U,
        .far_admissions_while_near_pending = 1U,
    }), start + 1s);
    tracker.observe(oneNearTile({
        .center = { 2, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = true,
        .accepted_near_admissions = 1U,
        .accepted_far_admissions = 1U,
        .far_admissions_while_near_pending = 1U,
    }), start + 1100ms);

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
    tracker.observe(oneNearTile({
        .center = { 0, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = false,
        .accepted_near_admissions = 1U,
    }), start);
    tracker.observe(oneNearTile({
        .center = { 1, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = false,
        .accepted_near_admissions = 1U,
    }), start + 4s + 1ms);
    tracker.observe(oneNearTile({
        .center = { 2, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = true,
        .accepted_near_admissions = 1U,
    }), start + 5s);

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
    tracker.observe(oneNearTile({
        .center = { 0, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = false,
        .accepted_near_admissions = 1U,
    }), start);
    tracker.observe(oneNearTile({
        .center = { 1, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = true,
        .accepted_near_admissions = 1U,
    }), start + 1s);

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
    acceptance::GameBenchmarkTerrainObservation first = oneNearTile({
        .center = { 0, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = false,
        .accepted_near_admissions = 1U,
    });
    tracker.observe(first, start);

    acceptance::GameBenchmarkTerrainObservation second = oneNearTile({
        .center = { 1, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = false,
        .accepted_near_admissions = 2U,
    });
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


TEST(GameBenchmark, RecoveryBarrierMustRemainCloserToTravelEndThanTravelStart)
{
    static constexpr shared::HeightTileKey MOVEMENT_START{ .x = 2'056, .y = 2'048 };
    static constexpr shared::HeightTileKey MOVEMENT_END{ .x = 2'441, .y = 2'048 };
    static constexpr shared::HeightTileKey RECOVERED_AT_DESTINATION{ .x = 2'440, .y = 2'048 };
    static constexpr shared::HeightTileKey RECOVERED_AT_ORIGIN{ .x = 2'056, .y = 2'048 };
    static constexpr shared::HeightTileKey WRAPPED_START{ .x = 4'095, .y = 0 };
    static constexpr shared::HeightTileKey WRAPPED_END{ .x = 1, .y = 0 };
    static constexpr shared::HeightTileKey WRAPPED_DESTINATION{ .x = 1, .y = 0 };

    EXPECT_TRUE(acceptance::gameBenchmarkRecoveryIsCloserToMovementEnd(
        MOVEMENT_START, MOVEMENT_END, RECOVERED_AT_DESTINATION
    ));
    EXPECT_FALSE(acceptance::gameBenchmarkRecoveryIsCloserToMovementEnd(
        MOVEMENT_START, MOVEMENT_END, RECOVERED_AT_ORIGIN
    ));
    EXPECT_TRUE(acceptance::gameBenchmarkRecoveryIsCloserToMovementEnd(
        WRAPPED_START, WRAPPED_END, WRAPPED_DESTINATION
    ));
    EXPECT_FALSE(acceptance::gameBenchmarkRecoveryIsCloserToMovementEnd(
        MOVEMENT_START, MOVEMENT_START, MOVEMENT_START
    ));
}


TEST(GameBenchmark, RetainsCurrentNearCoverageLossAfterLaterRecovery)
{
    using Clock = std::chrono::steady_clock;

    acceptance::GameBenchmarkTerrainReadinessTracker tracker;
    Clock::time_point const start{};
    tracker.observe(fullNearWindow({ 0, 0 }, acceptance::GAME_BENCHMARK_TERRAIN_NEAR_TILE_COUNT), start);
    tracker.observe(fullNearWindow({ 1, 0 }, 37U), start + 1s);
    tracker.observe(fullNearWindow({ 2, 0 }, acceptance::GAME_BENCHMARK_TERRAIN_NEAR_TILE_COUNT), start + 2s);

    acceptance::GameBenchmarkTerrainReadinessEvidence const evidence = tracker.evidence();
    EXPECT_EQ(evidence.current_near_sample_count, 3U);
    EXPECT_EQ(evidence.incomplete_current_near_sample_count, 1U);
    EXPECT_EQ(evidence.max_missing_current_mesh_count, 188U);
    EXPECT_FALSE(evidence.current_near_coverage_complete);
}
