#include <shared/world/WorldGenerationScheduler.hpp>

#include <gtest/gtest.h>

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
