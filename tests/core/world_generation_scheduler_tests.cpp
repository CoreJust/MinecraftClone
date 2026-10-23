#include <shared/world/Chunk.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>
#include <generation/Generation.hpp>

#include <gtest/gtest.h>

#include <array>
#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

template <typename Value>
Value const& requiredValue(std::optional<Value> const& optional)
{
    if (optional.has_value()) {
        return *optional;
    }
    throw std::runtime_error("Test expected an optional value");
}

std::shared_ptr<shared::TerrainWorld const> preparedTerrainWorld()
{
    static std::shared_ptr<shared::TerrainWorld const> const world = [] {
        generation::GenerationConfiguration configuration;
        configuration.seed = 8'800'555U;
        configuration.resolution = 256U;
        configuration.epochs = 1U;
        configuration.erosion_steps = 0U;
        configuration.initial_climate_iterations = 1U;
        configuration.climate_iterations = 1U;
        configuration.currents = false;
        configuration.coastal_elaboration = false;
        return std::make_shared<shared::TerrainWorld>(generation::prepareAtlas(configuration), 17U);
    }();
    return world;
}

shared::WorldGenerationPlan typedPlan()
{
    return shared::WorldGenerationPlan{
        .pre_generate = true,
        .refinement_extents = {4'096U, 1'024U, 256U, 64U},
        .chunk_stages = {shared::GenerationStage::HeightTile, shared::GenerationStage::Materialize},
    };
}

std::shared_ptr<shared::RegionalArtifact const> refineJob(
    shared::TerrainWorld const& world,
    shared::GenerationJob const& job
)
{
    if (!job.region.has_value()) {
        throw std::runtime_error{"Expected a typed regional job"};
    }
    return world.refineRegion(
        {
            .origin_x = static_cast<uint32_t>(job.region->origin_x),
            .origin_y = static_cast<uint32_t>(job.region->origin_y),
            .extent = job.region->extent,
        },
        job.inherited_region,
        job.cancellation_source == nullptr ? std::stop_token{} : job.cancellation_source->get_token()
    );
}

} // namespace

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
    EXPECT_EQ(scheduler.pendingCount(), 0U);
    ASSERT_EQ(
        scheduler.submit(SECOND, 1U, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::QueueFull
    );

    ASSERT_TRUE(scheduler.complete(running->id, true));
    EXPECT_EQ(scheduler.pendingCount(), 0U);
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
    EXPECT_EQ(requiredValue(job).stage, shared::GenerationStage::Materialize);
    EXPECT_FALSE(requiredValue(job).inherited_ancestor_data);
    ASSERT_TRUE(coordinator.complete(requiredValue(job).id, true));
    ASSERT_TRUE(coordinator.takeResult().has_value());
    ASSERT_TRUE(coordinator.acceptResult(requiredValue(job).id));
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
    EXPECT_EQ(coordinator.pendingCount(), 0U);
    static constexpr shared::ChunkCoordinate NEXT_COORDINATE{.x = 2, .y = 1, .z = 1};
    ASSERT_EQ(coordinator.request(NEXT_COORDINATE, {shared::GenerationStage::Materialize}),
        shared::GenerationAdmission::Accepted);
    EXPECT_FALSE(coordinator.requestRetry(COORDINATE, WORLD_REVISION, WORLD_SEED));
    EXPECT_FALSE(coordinator.complete(retry->id, true));
    ASSERT_TRUE(coordinator.cancel(NEXT_COORDINATE, WORLD_REVISION, WORLD_SEED));
    ASSERT_EQ(coordinator.request(COORDINATE, {shared::GenerationStage::Materialize}),
        shared::GenerationAdmission::Accepted);
    auto const fresh = coordinator.takeNext();
    ASSERT_TRUE(fresh.has_value());
    EXPECT_NE(fresh->id, retry->id);
    EXPECT_GT(fresh->attempt, retry->attempt);
    EXPECT_EQ(fresh->retries, 0U);
    EXPECT_FALSE(coordinator.complete(retry->id, true));

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
    ASSERT_TRUE(scheduler.complete(requiredValue(job).id, false));
    ASSERT_EQ(scheduler.resultCount(), 1U);
    ASSERT_TRUE(scheduler.takeResult().has_value());
    ASSERT_TRUE(scheduler.retry(requiredValue(job).id));
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
    EXPECT_EQ(requiredValue(scheduler.takeResult()).job.id, dispatched->id);
    EXPECT_EQ(requiredValue(scheduler.takeNext()).coordinate.x, 2);
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
        EXPECT_EQ(requiredValue(job).coordinate, coordinate);
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
            return std::tuple{ distance_squared - int64_t{ 2 } * job.coordinate.x, distance_squared };
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
            return std::tuple{ distance_squared - int64_t{ 2 } * job.coordinate.x, distance_squared };
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
        .refinement_extents = {},
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
    ASSERT_EQ(requiredValue(job).stage, shared::GenerationStage::Pregen);
    ASSERT_TRUE(requiredValue(job).region.has_value());
    EXPECT_EQ(requiredValue(requiredValue(job).region).extent, 16'384U);
    EXPECT_FALSE(requiredValue(job).inherited_ancestor_data);

    ASSERT_TRUE(coordinator.complete(requiredValue(job).id, true, false, {0x10U, 0x20U}));
    auto result = coordinator.takeResult();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded);
    ASSERT_TRUE(coordinator.acceptResult(requiredValue(job).id));

    job = coordinator.takeNext();
    ASSERT_TRUE(job.has_value());
    ASSERT_EQ(requiredValue(job).stage, shared::GenerationStage::Refinement);
    ASSERT_TRUE(requiredValue(job).region.has_value());
    EXPECT_EQ(requiredValue(requiredValue(job).region).extent, 4'096U);
    ASSERT_TRUE(requiredValue(job).inherited_ancestor_data);
    EXPECT_EQ(*requiredValue(job).inherited_ancestor_data, (std::vector<uint8_t>{0x10U, 0x20U}));
    ASSERT_TRUE(coordinator.complete(requiredValue(job).id, true, false, {0x30U}));
    ASSERT_TRUE(coordinator.takeResult().has_value());
    ASSERT_TRUE(coordinator.acceptResult(requiredValue(job).id));

    job = coordinator.takeNext();
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(requiredValue(requiredValue(job).region).extent, 1'024U);
    ASSERT_TRUE(requiredValue(job).inherited_ancestor_data);
    EXPECT_EQ(*requiredValue(job).inherited_ancestor_data, (std::vector<uint8_t>{0x30U}));
    std::vector<uint8_t> final_output(FINAL_OUTPUT_BYTES);
    final_output[1U] = 0xFCU;
    final_output[9U] = 0x04U;
    for (uint32_t offset = 12U; offset < FINAL_OUTPUT_BYTES; offset += 2U) {
        final_output[offset] = 64U;
    }
    ASSERT_TRUE(coordinator.complete(requiredValue(job).id, true, false, final_output));
    ASSERT_TRUE(coordinator.takeResult().has_value());
    ASSERT_TRUE(coordinator.acceptResult(requiredValue(job).id));

    job = coordinator.takeNext();
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(requiredValue(job).stage, shared::GenerationStage::HeightTile);
    EXPECT_FALSE(requiredValue(job).region.has_value());
    ASSERT_TRUE(coordinator.complete(requiredValue(job).id, true));
    ASSERT_TRUE(coordinator.takeResult().has_value());
    ASSERT_TRUE(coordinator.acceptResult(requiredValue(job).id));

    job = coordinator.takeNext();
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(requiredValue(job).stage, shared::GenerationStage::Materialize);
    ASSERT_TRUE(requiredValue(job).inherited_ancestor_data);
    EXPECT_EQ(*requiredValue(job).inherited_ancestor_data, final_output);
    shared::TerrainGenerator const generator;
    EXPECT_EQ(generator.generateChunk(TARGET).blockAt({.x = 0U, .y = 0U, .z = 0U}), shared::Block::Air);
    EXPECT_EQ(generator.generateChunk(TARGET, *requiredValue(job).inherited_ancestor_data)
        .blockAt({.x = 0U, .y = 0U, .z = 0U}), shared::Block::Stone);
    ASSERT_TRUE(coordinator.complete(requiredValue(job).id, true));
    ASSERT_TRUE(coordinator.takeResult().has_value());
    ASSERT_TRUE(coordinator.acceptResult(requiredValue(job).id));
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
    EXPECT_EQ(requiredValue(scheduler.takeNext()).coordinate.x, 1);
    EXPECT_EQ(requiredValue(scheduler.takeNext()).coordinate.x, 2);
    EXPECT_EQ(requiredValue(scheduler.takeNext()).coordinate.x, 0);
}

TEST(WorldGenerationSchedulerTest, QueuedWorldPipelinesFollowTheLatestPlayerPriority)
{
    static constexpr uint64_t REVISION = 7U;
    static constexpr uint64_t SEED = 89U;
    static constexpr shared::ChunkCoordinate FORMER_NEAR{.x = 1, .y = 0, .z = 0};
    static constexpr shared::ChunkCoordinate CURRENT_NEAR{.x = 9, .y = 0, .z = 0};
    shared::WorldGenerationCoordinator coordinator{4U, REVISION, SEED};
    shared::WorldGenerationPlan const plan{
        .refinement_extents = {},
        .chunk_stages = {shared::GenerationStage::Materialize},
    };

    ASSERT_EQ(
        coordinator.requestPlan(FORMER_NEAR, plan, {.distance = 0U}),
        shared::GenerationAdmission::Accepted
    );
    ASSERT_EQ(
        coordinator.requestPlan(CURRENT_NEAR, plan, {.distance = 10U}),
        shared::GenerationAdmission::Accepted
    );

    EXPECT_TRUE(coordinator.updatePriority(
        FORMER_NEAR,
        REVISION,
        SEED,
        {.distance = 20U}
    ));
    EXPECT_TRUE(coordinator.updatePriority(
        CURRENT_NEAR,
        REVISION,
        SEED,
        {.distance = 0U}
    ));

    auto const next = coordinator.takeNext();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->coordinate, CURRENT_NEAR);
}

TEST(WorldGenerationSchedulerTest, PreparedPipelinesShareExactTypedParentsThroughTheLeaf)
{
    static constexpr uint64_t WORLD_EPOCH = 13U;
    static constexpr shared::ChunkCoordinate FIRST{.x = 32, .y = 32, .z = 24};
    static constexpr shared::ChunkCoordinate SECOND{.x = 33, .y = 32, .z = 24};
    std::shared_ptr<shared::TerrainWorld const> const world = preparedTerrainWorld();
    shared::WorldGenerationCoordinator coordinator{
        8U,
        world->identity().revision,
        world->identity().seed,
    };
    coordinator.setTerrainContext(world, WORLD_EPOCH);

    EXPECT_EQ(coordinator.requestPlan(FIRST, typedPlan()), shared::GenerationAdmission::Accepted);
    EXPECT_EQ(coordinator.requestPlan(SECOND, typedPlan()), shared::GenerationAdmission::Accepted);

    std::shared_ptr<shared::RegionalArtifact const> parent;
    for (uint32_t const extent : shared::REGISTERED_WORLD_GENERATION_EXTENTS) {
        auto job = coordinator.takeNext();
        ASSERT_TRUE(job.has_value());
        ASSERT_TRUE(job->region.has_value());
        EXPECT_EQ(job->region->extent, extent);
        EXPECT_EQ(job->terrain_identity, world->identity());
        EXPECT_EQ(job->world_epoch, WORLD_EPOCH);
        EXPECT_NE(job->attempt, 0U);
        ASSERT_NE(job->cancellation_source, nullptr);
        EXPECT_EQ(job->inherited_region, parent);

        std::shared_ptr<shared::RegionalArtifact const> const artifact = refineJob(*world, *job);
        EXPECT_EQ(artifact->parent(), parent);
        shared::GenerationJob stale_attempt = *job;
        ++stale_attempt.attempt;
        EXPECT_FALSE(coordinator.completeRegional(stale_attempt, true, false, artifact));
        ASSERT_TRUE(coordinator.completeRegional(*job, true, false, artifact));
        auto result = coordinator.takeResult();
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->succeeded);
        EXPECT_EQ(result->regional_artifact, artifact);
        ASSERT_TRUE(coordinator.acceptResult(job->id));
        parent = artifact;
    }

    ASSERT_EQ(parent->key().extent, 64U);

    shared::WorldGenerationScheduler preview_scheduler{2U};
    static constexpr uint64_t PREVIEW_GENERATION = 29U;
    ASSERT_EQ(
        preview_scheduler.submit(FIRST, PREVIEW_GENERATION, shared::GenerationStage::HeightTile),
        shared::GenerationAdmission::Accepted
    );
    auto const unbound_preview = preview_scheduler.takeNext();
    ASSERT_TRUE(unbound_preview.has_value());
    auto const bound_preview = coordinator.bindTerrainLeaf(*unbound_preview);
    ASSERT_TRUE(bound_preview.has_value());
    EXPECT_EQ(bound_preview->revision, PREVIEW_GENERATION);
    EXPECT_EQ(bound_preview->seed, world->identity().seed);
    EXPECT_EQ(bound_preview->world_epoch, WORLD_EPOCH);
    EXPECT_EQ(bound_preview->inherited_region, parent);
    shared::GenerationJob stale_preview = *bound_preview;
    ++stale_preview.attempt;
    EXPECT_FALSE(preview_scheduler.complete(stale_preview, true));
    ASSERT_TRUE(preview_scheduler.complete(*bound_preview, true));
    auto const preview_result = preview_scheduler.takeResult();
    ASSERT_TRUE(preview_result.has_value());
    EXPECT_TRUE(preview_result->succeeded);

    std::vector<shared::GenerationJob> chunk_jobs;
    for (uint32_t index = 0U; index < 2U; ++index) {
        auto job = coordinator.takeNext();
        ASSERT_TRUE(job.has_value());
        EXPECT_EQ(job->stage, shared::GenerationStage::HeightTile);
        EXPECT_EQ(job->inherited_region, parent);
        EXPECT_EQ(job->terrain_identity, world->identity());
        EXPECT_EQ(job->world_epoch, WORLD_EPOCH);
        chunk_jobs.push_back(*job);
    }
    for (shared::GenerationJob const& job : chunk_jobs) {
        ASSERT_TRUE(coordinator.complete(job, true));
        ASSERT_TRUE(coordinator.takeResult().has_value());
        ASSERT_TRUE(coordinator.acceptResult(job.id));
    }

    chunk_jobs.clear();
    for (uint32_t index = 0U; index < 2U; ++index) {
        auto job = coordinator.takeNext();
        ASSERT_TRUE(job.has_value());
        EXPECT_EQ(job->stage, shared::GenerationStage::Materialize);
        EXPECT_EQ(job->inherited_region, parent);
        chunk_jobs.push_back(*job);
    }
}

TEST(WorldGenerationSchedulerTest, TypedContextRequiresTheCompleteExactRefinementPlan)
{
    std::shared_ptr<shared::TerrainWorld const> const world = preparedTerrainWorld();
    shared::WorldGenerationCoordinator coordinator{
        8U,
        world->identity().revision,
        world->identity().seed,
    };
    coordinator.setTerrainContext(world, 1U);

    shared::WorldGenerationPlan incomplete = typedPlan();
    incomplete.refinement_extents.pop_back();
    EXPECT_EQ(
        coordinator.requestPlan({.x = 32, .y = 32, .z = 24}, std::move(incomplete)),
        shared::GenerationAdmission::InvalidPlan
    );
    EXPECT_EQ(
        coordinator.requestPlan({.x = 32, .y = 32, .z = 24}, typedPlan()),
        shared::GenerationAdmission::Accepted
    );
}

TEST(WorldGenerationSchedulerTest, TypedRegionalJobsCannotCompleteThroughTheLegacyBytePath)
{
    std::shared_ptr<shared::TerrainWorld const> const world = preparedTerrainWorld();
    shared::WorldGenerationCoordinator coordinator{
        8U,
        world->identity().revision,
        world->identity().seed,
    };
    coordinator.setTerrainContext(world, 3U);
    ASSERT_EQ(
        coordinator.requestPlan({.x = 32, .y = 32, .z = 24}, typedPlan()),
        shared::GenerationAdmission::Accepted
    );

    auto const job = coordinator.takeNext();
    ASSERT_TRUE(job.has_value());
    ASSERT_EQ(job->stage, shared::GenerationStage::Pregen);
    EXPECT_FALSE(coordinator.complete(*job, true));
    EXPECT_FALSE(coordinator.complete(job->id, true));

    std::shared_ptr<shared::RegionalArtifact const> const artifact = refineJob(*world, *job);
    ASSERT_TRUE(coordinator.completeRegional(*job, true, false, artifact));
    auto result = coordinator.takeResult();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->succeeded);
    EXPECT_EQ(result->regional_artifact, artifact);
    EXPECT_TRUE(coordinator.acceptResult(job->id));
}

TEST(WorldGenerationSchedulerTest, EpochChangeCancelsAndDrainsAnInFlightRegionalCompletion)
{
    static constexpr uint64_t OLD_WORLD_EPOCH = 7U;
    static constexpr uint64_t NEW_WORLD_EPOCH = 8U;
    std::shared_ptr<shared::TerrainWorld const> const world = preparedTerrainWorld();
    shared::WorldGenerationCoordinator coordinator{
        8U,
        world->identity().revision,
        world->identity().seed,
    };
    coordinator.setTerrainContext(world, OLD_WORLD_EPOCH);
    ASSERT_EQ(
        coordinator.requestPlan({.x = 32, .y = 32, .z = 24}, typedPlan()),
        shared::GenerationAdmission::Accepted
    );

    auto const job = coordinator.takeNext();
    ASSERT_TRUE(job.has_value());
    ASSERT_NE(job->cancellation_source, nullptr);
    coordinator.setTerrainContext(world, NEW_WORLD_EPOCH);
    EXPECT_TRUE(job->cancellation_source->stop_requested());
    EXPECT_TRUE(coordinator.completeRegional(*job, false, true, nullptr));
    EXPECT_FALSE(coordinator.takeResult().has_value());
    EXPECT_EQ(coordinator.regionalArtifactBytes(), 0U);
}

TEST(WorldGenerationSchedulerTest, CancellingOnePipelineDetachesOnlyThatRegionalSubscriber)
{
    static constexpr uint64_t WORLD_EPOCH = 21U;
    static constexpr shared::ChunkCoordinate FIRST{.x = 32, .y = 32, .z = 24};
    static constexpr shared::ChunkCoordinate SECOND{.x = 33, .y = 32, .z = 24};
    std::shared_ptr<shared::TerrainWorld const> const world = preparedTerrainWorld();
    shared::WorldGenerationCoordinator coordinator{
        8U,
        world->identity().revision,
        world->identity().seed,
    };
    coordinator.setTerrainContext(world, WORLD_EPOCH);
    ASSERT_EQ(coordinator.requestPlan(FIRST, typedPlan()), shared::GenerationAdmission::Accepted);
    ASSERT_EQ(coordinator.requestPlan(SECOND, typedPlan()), shared::GenerationAdmission::Accepted);

    auto const shared_parent_job = coordinator.takeNext();
    ASSERT_TRUE(shared_parent_job.has_value());
    ASSERT_NE(shared_parent_job->cancellation_source, nullptr);
    EXPECT_TRUE(coordinator.cancel(FIRST, world->identity().revision, world->identity().seed));
    EXPECT_FALSE(shared_parent_job->cancellation_source->stop_requested());
    EXPECT_EQ(
        coordinator.state(SECOND, world->identity().revision, world->identity().seed),
        shared::GenerationState::Running
    );

    EXPECT_TRUE(coordinator.cancel(SECOND, world->identity().revision, world->identity().seed));
    EXPECT_TRUE(shared_parent_job->cancellation_source->stop_requested());
    EXPECT_TRUE(coordinator.completeRegional(*shared_parent_job, false, true, nullptr));
    EXPECT_FALSE(coordinator.takeResult().has_value());
    EXPECT_EQ(coordinator.regionalArtifactBytes(), 0U);
}

TEST(WorldGenerationSchedulerTest, TypedPipelinesProgressAcrossFourInterestsWithinRetentionBudget)
{
    static constexpr uint64_t WORLD_EPOCH = 31U;
    static constexpr uint64_t CAPACITY = 192U;
    static constexpr uint32_t MAX_ROUNDS = 128U;
    static constexpr std::array<int32_t, 2U> PLAYER_CENTERS{16'384, 49'152};
    static constexpr int32_t INTEREST_OFFSET = 512;
    static constexpr int32_t REGION_SPACING = 256;
    static constexpr int32_t SAMPLE_SPACING = 64;
    static constexpr int32_t CHUNK_Z = 24;
    static constexpr uint32_t REGIONS_PER_AXIS = 4U;
    static constexpr uint32_t SAMPLES_PER_REGION = 3U;

    auto const world = preparedTerrainWorld();
    shared::WorldGenerationCoordinator coordinator{CAPACITY, world->identity().revision, world->identity().seed};
    coordinator.setTerrainContext(world, WORLD_EPOCH);
    std::vector<shared::ChunkCoordinate> coordinates;
    coordinates.reserve(CAPACITY);
    for (int32_t const center_y : PLAYER_CENTERS) {
        for (int32_t const center_x : PLAYER_CENTERS) {
            for (uint32_t row = 0U; row < REGIONS_PER_AXIS; ++row) {
                for (uint32_t col = 0U; col < REGIONS_PER_AXIS; ++col) {
                    for (uint32_t sample = 1U; sample <= SAMPLES_PER_REGION; ++sample) {
                        int32_t const world_x = center_x - INTEREST_OFFSET
                            + static_cast<int32_t>(col) * REGION_SPACING
                            + static_cast<int32_t>(sample) * SAMPLE_SPACING;
                        int32_t const world_y = center_y - INTEREST_OFFSET
                            + static_cast<int32_t>(row) * REGION_SPACING + REGION_SPACING / 2;
                        shared::ChunkCoordinate const coordinate{
                            .x = world_x / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH),
                            .y = world_y / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH),
                            .z = CHUNK_Z,
                        };
                        ASSERT_EQ(coordinator.requestPlan(coordinate, typedPlan()),
                            shared::GenerationAdmission::Accepted);
                        coordinates.push_back(coordinate);
                    }
                }
            }
        }
    }
    ASSERT_EQ(coordinates.size(), CAPACITY);

    std::vector<std::weak_ptr<shared::RegionalArtifact const>> produced_regions;
    uint64_t terminal_count = 0U;
    for (uint32_t round = 0U; round < MAX_ROUNDS && terminal_count != CAPACITY; ++round) {
        std::vector<shared::GenerationJob> jobs;
        while (auto job = coordinator.takeNext()) {
            jobs.push_back(std::move(*job));
            EXPECT_LE(coordinator.regionalArtifactBytes(), shared::MAX_GENERATION_REGIONAL_RETENTION_BUDGET_BYTES);
        }
        ASSERT_FALSE(jobs.empty()) << "accepted terrain pipelines stopped making progress at round " << round;
        for (auto const& job : jobs) {
            if (job.region.has_value()) {
                auto artifact = refineJob(*world, job);
                produced_regions.push_back(artifact);
                ASSERT_TRUE(coordinator.completeRegional(job, true, false, std::move(artifact)));
            } else {
                ASSERT_TRUE(coordinator.complete(job, true));
            }
            {
                auto result = coordinator.takeResult();
                ASSERT_TRUE(result.has_value());
                ASSERT_TRUE(result->succeeded);
                ASSERT_TRUE(coordinator.acceptResult(job.id));
            }
            EXPECT_LE(coordinator.regionalArtifactBytes(), shared::MAX_GENERATION_REGIONAL_RETENTION_BUDGET_BYTES);
        }
        terminal_count = 0U;
        for (auto const coordinate : coordinates) {
            auto const state = coordinator.state(coordinate, world->identity().revision, world->identity().seed);
            terminal_count += state == shared::GenerationState::Materialized
                || state == shared::GenerationState::Failed;
        }
    }
    EXPECT_EQ(terminal_count, CAPACITY);
    for (auto const coordinate : coordinates) {
        EXPECT_EQ(coordinator.state(coordinate, world->identity().revision, world->identity().seed),
            shared::GenerationState::Materialized);
    }
    EXPECT_FALSE(coordinator.takeNext().has_value());
    EXPECT_EQ(coordinator.pendingCount(), 0U);
    EXPECT_EQ(coordinator.resultCount(), 0U);

    coordinator.setWorldIdentity(world->identity().revision + 1U, world->identity().seed);
    EXPECT_EQ(coordinator.regionalArtifactBytes(), 0U);
    for (auto const& artifact : produced_regions) {
        EXPECT_TRUE(artifact.expired());
    }
}

TEST(WorldGenerationSchedulerTest, CancelsTypedPipelineWaitingForCancelledWorkerCapacity)
{
    static constexpr uint64_t WORLD_EPOCH = 32U;
    static constexpr uint64_t CAPACITY = 1U;
    static constexpr shared::ChunkCoordinate COORDINATE{.x = 32, .y = 32, .z = 24};

    auto const world = preparedTerrainWorld();
    shared::WorldGenerationCoordinator coordinator{CAPACITY, world->identity().revision, world->identity().seed};
    coordinator.setTerrainContext(world, WORLD_EPOCH);
    ASSERT_EQ(coordinator.requestPlan(COORDINATE, typedPlan()), shared::GenerationAdmission::Accepted);
    auto const cancelled_worker = coordinator.takeNext();
    ASSERT_TRUE(cancelled_worker.has_value());
    ASSERT_TRUE(coordinator.cancel(COORDINATE, world->identity().revision, world->identity().seed));
    ASSERT_EQ(coordinator.requestPlan(COORDINATE, typedPlan()), shared::GenerationAdmission::Accepted);
    ASSERT_EQ(coordinator.state(COORDINATE, world->identity().revision, world->identity().seed),
        shared::GenerationState::Queued);
    EXPECT_TRUE(coordinator.cancel(COORDINATE, world->identity().revision, world->identity().seed));
    EXPECT_EQ(coordinator.state(COORDINATE, world->identity().revision, world->identity().seed),
        shared::GenerationState::Cancelled);
    ASSERT_TRUE(coordinator.completeRegional(*cancelled_worker, false, true, nullptr));
    EXPECT_FALSE(coordinator.takeResult().has_value());
    EXPECT_FALSE(coordinator.takeNext().has_value());
    EXPECT_EQ(coordinator.regionalArtifactBytes(), 0U);

    EXPECT_EQ(coordinator.requestPlan(COORDINATE, typedPlan()), shared::GenerationAdmission::Accepted);
    auto const fresh = coordinator.takeNext();
    ASSERT_TRUE(fresh.has_value());
    EXPECT_NE(fresh->id, cancelled_worker->id);
}

TEST(WorldGenerationSchedulerTest, WaitingTypedAdmissionUsesTheLatestPriority)
{
    static constexpr uint64_t WORLD_EPOCH = 33U;
    static constexpr uint64_t CAPACITY = 2U;
    static constexpr shared::ChunkCoordinate FIRST{.x = 0, .y = 0, .z = 0};
    static constexpr shared::ChunkCoordinate SECOND{.x = 1'024, .y = 0, .z = 0};
    static constexpr shared::ChunkCoordinate NEAR{.x = 1, .y = 0, .z = 0};
    static constexpr shared::ChunkCoordinate FAR{.x = 1'025, .y = 0, .z = 0};

    auto const world = preparedTerrainWorld();
    shared::WorldGenerationCoordinator coordinator{CAPACITY, world->identity().revision, world->identity().seed};
    coordinator.setTerrainContext(world, WORLD_EPOCH);
    ASSERT_EQ(coordinator.requestPlan(FIRST, typedPlan()), shared::GenerationAdmission::Accepted);
    ASSERT_EQ(coordinator.requestPlan(SECOND, typedPlan()), shared::GenerationAdmission::Accepted);
    auto const first_worker = coordinator.takeNext();
    auto const second_worker = coordinator.takeNext();
    ASSERT_TRUE(first_worker.has_value());
    ASSERT_TRUE(second_worker.has_value());
    ASSERT_TRUE(coordinator.cancel(FIRST, world->identity().revision, world->identity().seed));
    ASSERT_TRUE(coordinator.cancel(SECOND, world->identity().revision, world->identity().seed));
    ASSERT_EQ(coordinator.requestPlan(NEAR, typedPlan(), {.distance = 10U}), shared::GenerationAdmission::Accepted);
    ASSERT_EQ(coordinator.requestPlan(FAR, typedPlan(), {.distance = 0U}), shared::GenerationAdmission::Accepted);
    ASSERT_TRUE(coordinator.updatePriority(NEAR, world->identity().revision, world->identity().seed, {.distance = 0U}));
    ASSERT_TRUE(coordinator.updatePriority(FAR, world->identity().revision, world->identity().seed, {.distance = 10U}));
    ASSERT_TRUE(coordinator.completeRegional(*first_worker, false, true, nullptr));
    auto const next = coordinator.takeNext();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->coordinate, NEAR);
    ASSERT_TRUE(coordinator.completeRegional(*second_worker, false, true, nullptr));
}

TEST(WorldGenerationSchedulerTest, CancelsTypedPipelinesWaitingForRegionalAdmission)
{
    static constexpr uint64_t WORLD_EPOCH = 34U;
    static constexpr uint32_t CAPACITY = 64U;

    auto const world = preparedTerrainWorld();
    shared::WorldGenerationCoordinator coordinator{CAPACITY, world->identity().revision, world->identity().seed};
    coordinator.setTerrainContext(world, WORLD_EPOCH);
    for (uint32_t x = 0U; x < CAPACITY; ++x) {
        ASSERT_EQ(coordinator.requestPlan({.x = static_cast<int32_t>(x), .y = 0, .z = 0}, typedPlan()),
            shared::GenerationAdmission::Accepted);
    }
    for (uint32_t x = 0U; x < CAPACITY; ++x) {
        shared::ChunkCoordinate const coordinate{.x = static_cast<int32_t>(x), .y = 0, .z = 0};
        EXPECT_TRUE(coordinator.cancel(coordinate, world->identity().revision, world->identity().seed));
        EXPECT_EQ(coordinator.state(coordinate, world->identity().revision, world->identity().seed),
            shared::GenerationState::Cancelled);
    }
    EXPECT_FALSE(coordinator.takeNext().has_value());
    EXPECT_EQ(coordinator.pendingCount(), 0U);
    EXPECT_EQ(coordinator.resultCount(), 0U);
    EXPECT_EQ(coordinator.regionalArtifactBytes(), 0U);
}

TEST(WorldGenerationSchedulerTest, ExhaustedTypedRegionalFailuresReleaseAdmissionForWaitingPipelines)
{
    static constexpr uint64_t WORLD_EPOCH = 35U;
    static constexpr uint32_t CAPACITY = 64U;
    static constexpr uint32_t MAX_ROUNDS = 32U;
    static constexpr uint32_t FAILING_EXTENT = 4'096U;

    auto const world = preparedTerrainWorld();
    shared::WorldGenerationCoordinator coordinator{CAPACITY, world->identity().revision, world->identity().seed};
    coordinator.setTerrainContext(world, WORLD_EPOCH);
    for (uint32_t x = 0U; x < CAPACITY; ++x) {
        ASSERT_EQ(coordinator.requestPlan({.x = static_cast<int32_t>(x * 4U), .y = 0, .z = 0}, typedPlan()),
            shared::GenerationAdmission::Accepted);
    }
    uint32_t failed_count = 0U;
    for (uint32_t round = 0U; round < MAX_ROUNDS && failed_count != CAPACITY; ++round) {
        std::vector<shared::GenerationJob> jobs;
        while (auto job = coordinator.takeNext()) {
            jobs.push_back(std::move(*job));
        }
        ASSERT_FALSE(jobs.empty());
        for (auto const& job : jobs) {
            ASSERT_TRUE(job.region.has_value());
            bool const succeeds = job.region->extent != FAILING_EXTENT;
            ASSERT_TRUE(coordinator.completeRegional(
                job,
                succeeds,
                false,
                succeeds ? refineJob(*world, job) : nullptr
            ));
            auto const result = coordinator.takeResult();
            ASSERT_TRUE(result.has_value());
            if (succeeds) {
                ASSERT_TRUE(coordinator.acceptResult(job.id));
            } else if (job.retries < shared::WorldGenerationScheduler::MAX_RETRIES) {
                ASSERT_TRUE(coordinator.retry(job.id));
            }
            EXPECT_LE(coordinator.regionalArtifactBytes(), shared::MAX_GENERATION_REGIONAL_RETENTION_BUDGET_BYTES);
        }
        failed_count = 0U;
        for (uint32_t x = 0U; x < CAPACITY; ++x) {
            failed_count += coordinator.state({.x = static_cast<int32_t>(x * 4U), .y = 0, .z = 0},
                world->identity().revision, world->identity().seed) == shared::GenerationState::Failed;
        }
    }
    EXPECT_EQ(failed_count, CAPACITY);
    EXPECT_FALSE(coordinator.takeNext().has_value());
    EXPECT_EQ(coordinator.pendingCount(), 0U);
    EXPECT_EQ(coordinator.resultCount(), 0U);
    coordinator.setWorldIdentity(world->identity().revision + 1U, world->identity().seed);
    EXPECT_EQ(coordinator.regionalArtifactBytes(), 0U);
}
