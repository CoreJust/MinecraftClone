#include <shared/world/WorldGenerationScheduler.hpp>

#include <gtest/gtest.h>

#include <cstdint>

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
