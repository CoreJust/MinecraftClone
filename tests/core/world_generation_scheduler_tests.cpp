#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <tuple>
#include <vector>

TEST(WorldGenerationSchedulerTest, AdmissionIsBoundedAndDuplicateAware)
{
    static constexpr uint64_t MAX_PENDING = 1U;
    static constexpr shared::ChunkCoordinate COORDINATE{.x = 1, .y = 2, .z = 3};

    shared::WorldGenerationScheduler scheduler{MAX_PENDING};

    EXPECT_EQ(
        scheduler.submit(COORDINATE, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );
    EXPECT_EQ(
        scheduler.submit(COORDINATE, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Duplicate
    );
    EXPECT_EQ(
        scheduler.submit({.x = 2, .y = 2, .z = 3}, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::QueueFull
    );
}

TEST(WorldGenerationSchedulerTest, RunningAndUndrainedResultsConsumeTheOutstandingLimit)
{
    static constexpr uint64_t MAX_OUTSTANDING = 1U;
    static constexpr shared::ChunkCoordinate FIRST{.x = 1, .y = 2, .z = 3};
    static constexpr shared::ChunkCoordinate SECOND{.x = 4, .y = 5, .z = 6};

    shared::WorldGenerationScheduler scheduler{MAX_OUTSTANDING};
    ASSERT_EQ(
        scheduler.submit(FIRST, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );
    auto const running = scheduler.takeNext();
    ASSERT_TRUE(running.has_value());
    ASSERT_EQ(
        scheduler.submit(SECOND, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::QueueFull
    );

    ASSERT_TRUE(scheduler.complete(running->id, true));
    ASSERT_EQ(
        scheduler.submit(SECOND, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::QueueFull
    );
    ASSERT_TRUE(scheduler.takeResult().has_value());
    EXPECT_EQ(
        scheduler.submit(SECOND, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );
}

TEST(WorldGenerationSchedulerTest, UndrainedResultsConsumeTheOutstandingLimit)
{
    static constexpr uint64_t MAX_OUTSTANDING = 1U;
    static constexpr shared::ChunkCoordinate FIRST{.x = 1, .y = 2, .z = 3};
    static constexpr shared::ChunkCoordinate SECOND{.x = 4, .y = 5, .z = 6};

    shared::WorldGenerationScheduler scheduler{MAX_OUTSTANDING};
    ASSERT_EQ(
        scheduler.submit(FIRST, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );
    auto const running = scheduler.takeNext();
    ASSERT_TRUE(running.has_value());
    ASSERT_TRUE(scheduler.complete(running->id, true));
    EXPECT_EQ(
        scheduler.submit(SECOND, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::QueueFull
    );
    ASSERT_TRUE(scheduler.takeResult().has_value());
    EXPECT_EQ(
        scheduler.submit(SECOND, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );
}

TEST(WorldGenerationSchedulerTest, CoordinatorAdvancesOnlyThroughConfiguredStages)
{
    static constexpr uint64_t MAX_OUTSTANDING = 2U;
    static constexpr uint64_t WORLD_REVISION = 7U;
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr shared::ChunkCoordinate COORDINATE{.x = 2, .y = 3, .z = 4};

    shared::WorldGenerationCoordinator coordinator{MAX_OUTSTANDING, WORLD_REVISION, WORLD_SEED};
    EXPECT_EQ(
        coordinator.request(COORDINATE, {
            shared::GenerationStage::HeightTile,
            shared::GenerationStage::Materialize,
        }),
        shared::GenerationAdmission::Accepted
    );
    EXPECT_EQ(
        coordinator.state(COORDINATE, WORLD_REVISION, WORLD_SEED),
        shared::GenerationState::Queued
    );

    auto const preview_job = coordinator.takeNext();
    ASSERT_TRUE(preview_job.has_value());
    EXPECT_EQ(preview_job->stage, shared::GenerationStage::HeightTile);
    EXPECT_EQ(preview_job->coordinate, COORDINATE);
    EXPECT_EQ(preview_job->revision, WORLD_REVISION);
    EXPECT_EQ(preview_job->seed, WORLD_SEED);
    EXPECT_EQ(
        coordinator.state(COORDINATE, WORLD_REVISION, WORLD_SEED),
        shared::GenerationState::Running
    );
    ASSERT_TRUE(coordinator.complete(preview_job->id, true));
    ASSERT_TRUE(coordinator.takeResult().has_value());
    EXPECT_EQ(
        coordinator.state(COORDINATE, WORLD_REVISION, WORLD_SEED),
        shared::GenerationState::Preview
    );
    ASSERT_TRUE(coordinator.acceptResult(preview_job->id));

    auto const materialization_job = coordinator.takeNext();
    ASSERT_TRUE(materialization_job.has_value());
    EXPECT_EQ(materialization_job->stage, shared::GenerationStage::Materialize);
    EXPECT_EQ(materialization_job->coordinate, COORDINATE);
    EXPECT_EQ(materialization_job->revision, WORLD_REVISION);
    EXPECT_EQ(materialization_job->seed, WORLD_SEED);
    ASSERT_TRUE(coordinator.complete(materialization_job->id, true));
    ASSERT_TRUE(coordinator.takeResult().has_value());
    ASSERT_TRUE(coordinator.acceptResult(materialization_job->id));
    EXPECT_EQ(
        coordinator.state(COORDINATE, WORLD_REVISION, WORLD_SEED),
        shared::GenerationState::Materialized
    );
}

TEST(WorldGenerationSchedulerTest, CoordinatorAllowsOmittingPreviewAndRejectsMaterializationBeforePreview)
{
    static constexpr uint64_t WORLD_REVISION = 3U;
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr shared::ChunkCoordinate COORDINATE{.x = 8, .y = 9, .z = 10};

    shared::WorldGenerationCoordinator coordinator{4U, WORLD_REVISION, WORLD_SEED};
    EXPECT_EQ(
        coordinator.request(COORDINATE, {shared::GenerationStage::Materialize}),
        shared::GenerationAdmission::Accepted
    );
    auto const job = coordinator.takeNext();
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->stage, shared::GenerationStage::Materialize);
    EXPECT_FALSE(job->inherited_ancestor_data);
    ASSERT_TRUE(coordinator.complete(job->id, true));
    ASSERT_TRUE(coordinator.takeResult().has_value());
    ASSERT_TRUE(coordinator.acceptResult(job->id));
    EXPECT_EQ(
        coordinator.state(COORDINATE, WORLD_REVISION, WORLD_SEED),
        shared::GenerationState::Materialized
    );

    EXPECT_EQ(
        coordinator.request({.x = 11, .y = 12, .z = 13}, {
            shared::GenerationStage::Materialize,
            shared::GenerationStage::HeightTile,
        }),
        shared::GenerationAdmission::InvalidPlan
    );
}

TEST(WorldGenerationSchedulerTest, CoordinatorRejectsStalePublicationAndBoundsExplicitRetries)
{
    static constexpr uint64_t WORLD_REVISION = 4U;
    static constexpr uint64_t WORLD_SEED = 99U;
    static constexpr shared::ChunkCoordinate COORDINATE{.x = 1, .y = 1, .z = 1};

    shared::WorldGenerationCoordinator coordinator{1U, WORLD_REVISION, WORLD_SEED};
    ASSERT_EQ(
        coordinator.request(COORDINATE, {shared::GenerationStage::Materialize}),
        shared::GenerationAdmission::Accepted
    );
    auto const first = coordinator.takeNext();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(
        coordinator.request({.x = 2, .y = 1, .z = 1}, {shared::GenerationStage::Materialize}),
        shared::GenerationAdmission::QueueFull
    );
    ASSERT_TRUE(coordinator.complete(first->id, false));
    ASSERT_TRUE(coordinator.takeResult().has_value());
    EXPECT_EQ(
        coordinator.state(COORDINATE, WORLD_REVISION, WORLD_SEED),
        shared::GenerationState::Failed
    );
    ASSERT_TRUE(coordinator.requestRetry(COORDINATE, WORLD_REVISION, WORLD_SEED));
    auto const retry = coordinator.takeNext();
    ASSERT_TRUE(retry.has_value());
    EXPECT_EQ(retry->retries, 1U);
    ASSERT_TRUE(coordinator.complete(retry->id, false));
    ASSERT_TRUE(coordinator.takeResult().has_value());
    EXPECT_FALSE(coordinator.requestRetry(COORDINATE, WORLD_REVISION, WORLD_SEED));
    EXPECT_FALSE(coordinator.requestRetry(COORDINATE, WORLD_REVISION + 1U, WORLD_SEED));

    coordinator.setWorldIdentity(WORLD_REVISION + 1U, WORLD_SEED);
    EXPECT_EQ(
        coordinator.state(COORDINATE, WORLD_REVISION, WORLD_SEED),
        shared::GenerationState::Cancelled
    );
    EXPECT_EQ(
        coordinator.state(COORDINATE, WORLD_REVISION + 1U, WORLD_SEED),
        shared::GenerationState::Unknown
    );
}

TEST(WorldGenerationSchedulerTest, CancelledIdentityCanBeRequestedAgain)
{
    static constexpr uint64_t WORLD_REVISION = 7U;
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr shared::ChunkCoordinate COORDINATE{.x = 2, .y = 3, .z = 4};

    shared::WorldGenerationCoordinator coordinator{1U, WORLD_REVISION, WORLD_SEED};
    ASSERT_EQ(
        coordinator.request(COORDINATE, {shared::GenerationStage::Materialize}),
        shared::GenerationAdmission::Accepted
    );
    ASSERT_TRUE(coordinator.cancel(COORDINATE, WORLD_REVISION, WORLD_SEED));
    EXPECT_EQ(coordinator.state(COORDINATE, WORLD_REVISION, WORLD_SEED), shared::GenerationState::Cancelled);
    ASSERT_EQ(
        coordinator.request(COORDINATE, {shared::GenerationStage::Materialize}),
        shared::GenerationAdmission::Accepted
    );
    auto const replacement = coordinator.takeNext();
    ASSERT_TRUE(replacement.has_value());
    ASSERT_TRUE(coordinator.complete(replacement->id, true));
    ASSERT_TRUE(coordinator.takeResult().has_value());
    ASSERT_TRUE(coordinator.acceptResult(replacement->id));
    EXPECT_EQ(coordinator.state(COORDINATE, WORLD_REVISION, WORLD_SEED), shared::GenerationState::Materialized);
}

TEST(WorldGenerationSchedulerTest, LateCancelledJobReleasesCapacityForSameIdentity)
{
    static constexpr uint64_t WORLD_REVISION = 7U;
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr shared::ChunkCoordinate COORDINATE{.x = 2, .y = 3, .z = 4};

    shared::WorldGenerationCoordinator coordinator{1U, WORLD_REVISION, WORLD_SEED};
    ASSERT_EQ(
        coordinator.request(COORDINATE, {shared::GenerationStage::Materialize}),
        shared::GenerationAdmission::Accepted
    );
    auto const cancelled = coordinator.takeNext();
    ASSERT_TRUE(cancelled.has_value());
    ASSERT_TRUE(coordinator.cancel(COORDINATE, WORLD_REVISION, WORLD_SEED));
    EXPECT_EQ(
        coordinator.request(COORDINATE, {shared::GenerationStage::Materialize}),
        shared::GenerationAdmission::Accepted
    );
    EXPECT_FALSE(coordinator.takeNext().has_value());
    ASSERT_TRUE(coordinator.complete(cancelled->id, true));
    EXPECT_FALSE(coordinator.takeResult().has_value());
    auto const replacement = coordinator.takeNext();
    ASSERT_TRUE(replacement.has_value());
    EXPECT_NE(replacement->id, cancelled->id);
}

TEST(WorldGenerationSchedulerTest, StaleAndCancelledResultsAreDiscarded)
{
    shared::WorldGenerationScheduler scheduler;
    ASSERT_EQ(
        scheduler.submit({.x = 1, .y = 2, .z = 3}, 9U, shared::GenerationStage::Materialize),
        shared::GenerationAdmission::Accepted
    );
    auto const stale_job = scheduler.takeNext();
    ASSERT_TRUE(stale_job.has_value());
    scheduler.invalidateRevision(9U);
    ASSERT_TRUE(scheduler.complete(stale_job->id, true));
    EXPECT_EQ(scheduler.resultCount(), 0U);

    ASSERT_EQ(
        scheduler.submit({.x = 4, .y = 5, .z = 6}, 10U, shared::GenerationStage::Materialize),
        shared::GenerationAdmission::Accepted
    );
    auto const cancelled_job = scheduler.takeNext();
    ASSERT_TRUE(cancelled_job.has_value());
    ASSERT_TRUE(scheduler.cancel(cancelled_job->id));
    ASSERT_TRUE(scheduler.complete(cancelled_job->id, true));
    EXPECT_EQ(scheduler.resultCount(), 0U);
}

TEST(WorldGenerationSchedulerTest, ExplicitRetryIsLimitedToOneAttempt)
{
    shared::WorldGenerationScheduler scheduler;
    ASSERT_EQ(
        scheduler.submit({.x = 7, .y = 8, .z = 9}, 11U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );
    auto const job = scheduler.takeNext();
    ASSERT_TRUE(job.has_value());
    ASSERT_TRUE(scheduler.complete(job->id, false));
    ASSERT_EQ(scheduler.resultCount(), 1U);
    ASSERT_TRUE(scheduler.takeResult().has_value());
    ASSERT_TRUE(scheduler.retry(job->id));
    auto const retry_job = scheduler.takeNext();
    ASSERT_TRUE(retry_job.has_value());
    ASSERT_TRUE(scheduler.complete(retry_job->id, false));
    EXPECT_FALSE(scheduler.retry(retry_job->id));
}

TEST(WorldGenerationSchedulerTest, ReprioritizingQueuedWorkPreservesDispatchedJob)
{
    shared::WorldGenerationScheduler scheduler{2U};
    ASSERT_EQ(scheduler.submit({.x = 1, .y = 0, .z = 0}, 1U, shared::GenerationStage::HeightTile),
              shared::GenerationAdmission::Accepted);
    auto const dispatched = scheduler.takeNext();
    ASSERT_TRUE(dispatched.has_value());
    ASSERT_EQ(scheduler.submit({.x = 2, .y = 0, .z = 0}, 1U, shared::GenerationStage::HeightTile),
              shared::GenerationAdmission::Accepted);
    scheduler.cancelQueued();
    EXPECT_EQ(scheduler.pendingCount(), 0U);
    EXPECT_EQ(scheduler.submit({.x = 2, .y = 0, .z = 0}, 1U, shared::GenerationStage::HeightTile),
              shared::GenerationAdmission::Accepted);
    ASSERT_TRUE(scheduler.complete(dispatched->id, true));
    EXPECT_EQ(scheduler.takeResult()->job.id, dispatched->id);
    EXPECT_EQ(scheduler.takeNext()->coordinate.x, 2);
}

TEST(WorldGenerationSchedulerTest, ReordersRowMajorSubmissionIntoNearestVisibleTiles)
{
    static constexpr int32_t WINDOW_RADIUS = 1;
    static constexpr uint64_t JOB_COUNT = 9U;

    shared::WorldGenerationScheduler scheduler{JOB_COUNT};
    for (int32_t y = -WINDOW_RADIUS; y <= WINDOW_RADIUS; ++y) {
        for (int32_t x = -WINDOW_RADIUS; x <= WINDOW_RADIUS; ++x) {
            ASSERT_EQ(
                scheduler.submit({.x = x, .y = y, .z = 0}, 1U, shared::GenerationStage::HeightTile),
                shared::GenerationAdmission::Accepted
            );
        }
    }

    scheduler.reorderQueued([](shared::GenerationJob const& first, shared::GenerationJob const& second) {
        auto const priority = [](shared::GenerationJob const& job) {
            int64_t const distance_squared = static_cast<int64_t>(job.coordinate.x) * job.coordinate.x
                + static_cast<int64_t>(job.coordinate.y) * job.coordinate.y;
            return std::tuple{ distance_squared, job.coordinate.y, job.coordinate.x };
        };
        return priority(first) < priority(second);
    });

    std::vector<shared::ChunkCoordinate> const expected{
        {.x = 0, .y = 0, .z = 0},
        {.x = 0, .y = -1, .z = 0},
        {.x = -1, .y = 0, .z = 0},
        {.x = 1, .y = 0, .z = 0},
        {.x = 0, .y = 1, .z = 0},
    };
    for (shared::ChunkCoordinate const coordinate : expected) {
        auto const job = scheduler.takeNext();
        ASSERT_TRUE(job.has_value());
        EXPECT_EQ(job->coordinate, coordinate);
    }
}

TEST(WorldGenerationSchedulerTest, MovingCenterKeepsDispatchedAndOverlappingQueuedWork)
{
    shared::WorldGenerationScheduler scheduler{4U};
    ASSERT_EQ(
        scheduler.submit({.x = 0, .y = 0, .z = 0}, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );
    auto const dispatched = scheduler.takeNext();
    ASSERT_TRUE(dispatched.has_value());
    ASSERT_EQ(
        scheduler.submit({.x = -45, .y = 0, .z = 0}, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );
    ASSERT_EQ(
        scheduler.submit({.x = -1, .y = 0, .z = 0}, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );
    ASSERT_EQ(
        scheduler.submit({.x = 1, .y = 0, .z = 0}, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );

    scheduler.cancelQueuedIf([](shared::GenerationJob const& job) {
        return job.coordinate.x == -45;
    });
    scheduler.reorderQueued([](shared::GenerationJob const& first, shared::GenerationJob const& second) {
        auto const priority = [](shared::GenerationJob const& job) {
            int64_t const distance_squared = static_cast<int64_t>(job.coordinate.x) * job.coordinate.x
                + static_cast<int64_t>(job.coordinate.y) * job.coordinate.y;
            return std::tuple{ distance_squared - 2 * job.coordinate.x, distance_squared };
        };
        return priority(first) < priority(second);
    });

    EXPECT_EQ(scheduler.pendingCount(), 2U);
    ASSERT_TRUE(scheduler.complete(dispatched->id, true));
    auto const dispatched_result = scheduler.takeResult();
    ASSERT_TRUE(dispatched_result.has_value());
    EXPECT_EQ(dispatched_result->job.id, dispatched->id);
    auto const ahead = scheduler.takeNext();
    ASSERT_TRUE(ahead.has_value());
    EXPECT_EQ(ahead->coordinate, (shared::ChunkCoordinate{.x = 1, .y = 0, .z = 0}));
    auto const retained = scheduler.takeNext();
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained->coordinate, (shared::ChunkCoordinate{.x = -1, .y = 0, .z = 0}));
}

TEST(WorldGenerationSchedulerTest, DistancePriorityOutranksFarForwardTile)
{
    shared::WorldGenerationScheduler scheduler{2U};
    ASSERT_EQ(
        scheduler.submit({.x = 45, .y = 0, .z = 0}, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );
    ASSERT_EQ(
        scheduler.submit({.x = 0, .y = 1, .z = 0}, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );

    scheduler.reorderQueued([](shared::GenerationJob const& first, shared::GenerationJob const& second) {
        auto const priority = [](shared::GenerationJob const& job) {
            int64_t const distance_squared = static_cast<int64_t>(job.coordinate.x) * job.coordinate.x
                + static_cast<int64_t>(job.coordinate.y) * job.coordinate.y;
            return std::tuple{ distance_squared - 2 * job.coordinate.x, distance_squared };
        };
        return priority(first) < priority(second);
    });

    auto const nearest = scheduler.takeNext();
    ASSERT_TRUE(nearest.has_value());
    EXPECT_EQ(nearest->coordinate, (shared::ChunkCoordinate{.x = 0, .y = 1, .z = 0}));
}

TEST(WorldGenerationSchedulerTest, NewRevisionDropsQueuedFrontierAndLateResults)
{
    shared::WorldGenerationScheduler scheduler{4U};
    ASSERT_EQ(
        scheduler.submit({.x = 0, .y = 0, .z = 0}, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );
    ASSERT_EQ(
        scheduler.submit({.x = 1, .y = 0, .z = 0}, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );
    ASSERT_EQ(
        scheduler.submit({.x = 0, .y = 1, .z = 0}, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );
    auto const stale_running = scheduler.takeNext();
    ASSERT_TRUE(stale_running.has_value());

    scheduler.invalidateRevision(1U);
    scheduler.cancelQueued();
    ASSERT_EQ(
        scheduler.submit({.x = 10, .y = 0, .z = 0}, 2U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );
    ASSERT_EQ(
        scheduler.submit({.x = 11, .y = 0, .z = 0}, 2U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );

    ASSERT_TRUE(scheduler.complete(stale_running->id, true));
    EXPECT_EQ(scheduler.resultCount(), 0U);
    auto const next = scheduler.takeNext();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->revision, 2U);
    EXPECT_EQ(next->coordinate, (shared::ChunkCoordinate{.x = 10, .y = 0, .z = 0}));
}

TEST(WorldGenerationSchedulerTest, RegisteredRefinementsNormalizeWrappedAndNegativeCoordinates)
{
    static constexpr uint64_t REVISION = 7U;
    static constexpr uint64_t SEED = 19U;
    static constexpr shared::ChunkCoordinate LAST_CHUNK{.x = 4'095, .y = 1, .z = 2};
    static constexpr shared::ChunkCoordinate NEGATIVE_CHUNK{.x = -1, .y = 1, .z = 2};

    EXPECT_EQ(
        shared::registeredWorldGenerationExtents(),
        (std::array<uint32_t, 5>{16'384U, 4'096U, 1'024U, 256U, 64U})
    );
    auto const wrapped = shared::generationRegionForChunk(
        LAST_CHUNK,
        64U,
        shared::GenerationStage::Refinement,
        REVISION,
        SEED
    );
    auto const negative = shared::generationRegionForChunk(
        NEGATIVE_CHUNK,
        64U,
        shared::GenerationStage::Refinement,
        REVISION,
        SEED
    );
    ASSERT_TRUE(wrapped.has_value());
    ASSERT_TRUE(negative.has_value());
    EXPECT_EQ(wrapped->origin_x, 65'472);
    EXPECT_EQ(negative->origin_x, wrapped->origin_x);
    EXPECT_EQ(negative->origin_y, wrapped->origin_y);
    EXPECT_EQ(negative->revision, REVISION);
    EXPECT_EQ(negative->seed, SEED);
}

TEST(WorldGenerationSchedulerTest, PlansValidateOptionalStagesExtentsAndPriorityDimensions)
{
    shared::WorldGenerationPlan preview_only{
        .chunk_stages = {shared::GenerationStage::HeightTile},
    };
    EXPECT_TRUE(shared::isValidWorldGenerationPlan(preview_only));

    shared::WorldGenerationPlan staged{
        .pre_generate = true,
        .refinement_extents = {4'096U, 1'024U, 256U, 64U},
        .chunk_stages = {shared::GenerationStage::Materialize},
        .priority_order = {
            shared::GenerationPriorityDimension::Movement,
            shared::GenerationPriorityDimension::Distance,
            shared::GenerationPriorityDimension::View,
        },
    };
    EXPECT_TRUE(shared::isValidWorldGenerationPlan(staged));

    staged.refinement_extents = {1'024U, 4'096U};
    EXPECT_FALSE(shared::isValidWorldGenerationPlan(staged));
    staged.refinement_extents = {4'096U, 4'096U};
    EXPECT_FALSE(shared::isValidWorldGenerationPlan(staged));
    staged.refinement_extents = {4'096U, 0U};
    EXPECT_FALSE(shared::isValidWorldGenerationPlan(staged));
    staged.refinement_extents = {4'096U};
    staged.priority_order = {
        shared::GenerationPriorityDimension::Distance,
        shared::GenerationPriorityDimension::Distance,
    };
    EXPECT_FALSE(shared::isValidWorldGenerationPlan(staged));
    staged.priority_order = {};
    staged.chunk_stages = {shared::GenerationStage::Materialize, shared::GenerationStage::HeightTile};
    EXPECT_FALSE(shared::isValidWorldGenerationPlan(staged));
    staged.chunk_stages.clear();
    staged.refinement_extents.clear();
    staged.pre_generate = false;
    EXPECT_FALSE(shared::isValidWorldGenerationPlan(staged));
}

TEST(WorldGenerationSchedulerTest, PlanChainsBoundedRefinementsAndPassesImmutableAncestorOutput)
{
    static constexpr uint64_t REVISION = 3U;
    static constexpr uint64_t SEED = 77U;
    static constexpr uint32_t FINAL_OUTPUT_BYTES = 12U + 16U * 16U * 2U;
    static constexpr shared::ChunkCoordinate TARGET{.x = 4'095, .y = 23, .z = 2};
    shared::WorldGenerationCoordinator coordinator{8U, REVISION, SEED};
    shared::WorldGenerationPlan const plan{
        .pre_generate = true,
        .refinement_extents = {4'096U, 1'024U},
        .chunk_stages = {shared::GenerationStage::HeightTile, shared::GenerationStage::Materialize},
        .priority_order = {
            shared::GenerationPriorityDimension::Movement,
            shared::GenerationPriorityDimension::Distance,
        },
    };

    ASSERT_EQ(coordinator.requestPlan(TARGET, plan, {.distance = 4U, .view = 1U, .movement = 0U}),
              shared::GenerationAdmission::Accepted);
    auto job = coordinator.takeNext();
    ASSERT_TRUE(job.has_value());
    ASSERT_EQ(job->stage, shared::GenerationStage::Pregen);
    ASSERT_TRUE(job->region.has_value());
    EXPECT_EQ(job->region->extent, 16'384U);
    EXPECT_FALSE(job->inherited_ancestor_data);

    ASSERT_TRUE(coordinator.complete(job->id, true, false, {0x10U, 0x20U}));
    auto result = coordinator.takeResult();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded);
    ASSERT_TRUE(coordinator.acceptResult(job->id));

    job = coordinator.takeNext();
    ASSERT_TRUE(job.has_value());
    ASSERT_EQ(job->stage, shared::GenerationStage::Refinement);
    ASSERT_TRUE(job->region.has_value());
    EXPECT_EQ(job->region->extent, 4'096U);
    ASSERT_TRUE(job->inherited_ancestor_data);
    EXPECT_EQ(*job->inherited_ancestor_data, (std::vector<uint8_t>{0x10U, 0x20U}));
    ASSERT_TRUE(coordinator.complete(job->id, true, false, {0x30U}));
    ASSERT_TRUE(coordinator.takeResult().has_value());
    ASSERT_TRUE(coordinator.acceptResult(job->id));

    job = coordinator.takeNext();
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->region->extent, 1'024U);
    ASSERT_TRUE(job->inherited_ancestor_data);
    EXPECT_EQ(*job->inherited_ancestor_data, (std::vector<uint8_t>{0x30U}));
    std::vector<uint8_t> final_output(FINAL_OUTPUT_BYTES);
    final_output[1U] = 0xFCU;
    final_output[9U] = 0x04U;
    for (uint32_t offset = 12U; offset < FINAL_OUTPUT_BYTES; offset += 2U) {
        final_output[offset] = 64U;
    }
    ASSERT_TRUE(coordinator.complete(job->id, true, false, final_output));
    ASSERT_TRUE(coordinator.takeResult().has_value());
    ASSERT_TRUE(coordinator.acceptResult(job->id));

    job = coordinator.takeNext();
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->stage, shared::GenerationStage::HeightTile);
    EXPECT_FALSE(job->region.has_value());
    ASSERT_TRUE(coordinator.complete(job->id, true));
    ASSERT_TRUE(coordinator.takeResult().has_value());
    ASSERT_TRUE(coordinator.acceptResult(job->id));

    job = coordinator.takeNext();
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->stage, shared::GenerationStage::Materialize);
    ASSERT_TRUE(job->inherited_ancestor_data);
    EXPECT_EQ(*job->inherited_ancestor_data, final_output);
    shared::TerrainGenerator const generator;
    EXPECT_EQ(generator.generateChunk(TARGET).blockAt({.x = 0U, .y = 0U, .z = 0U}), shared::Block::Air);
    EXPECT_EQ(generator.generateChunk(TARGET, *job->inherited_ancestor_data)
        .blockAt({.x = 0U, .y = 0U, .z = 0U}), shared::Block::Stone);
    ASSERT_TRUE(coordinator.complete(job->id, true));
    ASSERT_TRUE(coordinator.takeResult().has_value());
    ASSERT_TRUE(coordinator.acceptResult(job->id));
    EXPECT_EQ(coordinator.state(TARGET, REVISION, SEED), shared::GenerationState::Materialized);
}

TEST(WorldGenerationSchedulerTest, CanonicalRegionJobsDeduplicateAcrossTargetChunks)
{
    static constexpr uint64_t REVISION = 5U;
    static constexpr uint64_t SEED = 83U;
    static constexpr shared::ChunkCoordinate FIRST{.x = 1, .y = 3, .z = 0};
    static constexpr shared::ChunkCoordinate SECOND{.x = 2, .y = 3, .z = 0};
    shared::WorldGenerationPlan const plan{
        .refinement_extents = {4'096U},
        .chunk_stages = {shared::GenerationStage::Materialize},
    };
    shared::WorldGenerationCoordinator coordinator{4U, REVISION, SEED};

    ASSERT_EQ(coordinator.requestPlan(FIRST, plan), shared::GenerationAdmission::Accepted);
    auto const region_job = coordinator.takeNext();
    ASSERT_TRUE(region_job.has_value());
    ASSERT_EQ(region_job->stage, shared::GenerationStage::Refinement);
    ASSERT_EQ(coordinator.requestPlan(SECOND, plan), shared::GenerationAdmission::Duplicate);

    ASSERT_TRUE(coordinator.complete(region_job->id, true, false, {0x21U, 0x34U}));
    ASSERT_TRUE(coordinator.takeResult().has_value());
    ASSERT_TRUE(coordinator.acceptResult(region_job->id));
    ASSERT_EQ(coordinator.requestPlan(SECOND, plan), shared::GenerationAdmission::Accepted);

    auto const first_materialization = coordinator.takeNext();
    ASSERT_TRUE(first_materialization.has_value());
    ASSERT_EQ(first_materialization->stage, shared::GenerationStage::Materialize);
    ASSERT_EQ(first_materialization->coordinate, FIRST);
    auto const second_materialization = coordinator.takeNext();
    ASSERT_TRUE(second_materialization.has_value());
    ASSERT_EQ(second_materialization->stage, shared::GenerationStage::Materialize);
    ASSERT_EQ(second_materialization->coordinate, SECOND);
}

TEST(WorldGenerationSchedulerTest, GenerationPriorityDimensionsUseStableLexicographicOrder)
{
    shared::WorldGenerationScheduler scheduler{3U};
    ASSERT_EQ(
        scheduler.submit({.x = 0, .y = 0, .z = 0}, 1U, shared::GenerationStage::Materialize,
                         {.distance = 0U, .view = 0U, .movement = 2U}),
        shared::GenerationAdmission::Accepted
    );
    ASSERT_EQ(
        scheduler.submit({.x = 1, .y = 0, .z = 0}, 1U, shared::GenerationStage::Materialize,
                         {.distance = 0U, .view = 1U, .movement = 0U}),
        shared::GenerationAdmission::Accepted
    );
    ASSERT_EQ(
        scheduler.submit({.x = 2, .y = 0, .z = 0}, 1U, shared::GenerationStage::Materialize,
                         {.distance = 1U, .view = 0U, .movement = 0U}),
        shared::GenerationAdmission::Accepted
    );

    std::array<shared::GenerationPriorityDimension, 3> const priority_order{
        shared::GenerationPriorityDimension::Movement,
        shared::GenerationPriorityDimension::Distance,
        shared::GenerationPriorityDimension::View,
    };
    ASSERT_TRUE(scheduler.reorderQueued(priority_order));
    EXPECT_EQ(scheduler.takeNext()->coordinate.x, 1);
    EXPECT_EQ(scheduler.takeNext()->coordinate.x, 2);
    EXPECT_EQ(scheduler.takeNext()->coordinate.x, 0);
}
