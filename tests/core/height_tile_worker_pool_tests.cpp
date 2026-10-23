#include <server/HeightTileWorkerPool.hpp>
#include <shared/net/Message.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>
#include <generation/Generation.hpp>
#include <core/net/Event.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::shared_ptr<shared::TerrainWorld const> preparedTerrainWorld()
{
    static std::shared_ptr<shared::TerrainWorld const> const terrain_world = [] {
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
    return terrain_world;
}

std::vector<server::HeightTileWorkerPool::Result> takeResults(
    server::HeightTileWorkerPool& pool,
    uint32_t const expected_count
)
{
    std::vector<server::HeightTileWorkerPool::Result> results;
    results.reserve(expected_count);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (results.size() < expected_count && std::chrono::steady_clock::now() < deadline) {
        std::vector<server::HeightTileWorkerPool::Result> ready = pool.takeResults(
            expected_count - static_cast<uint32_t>(results.size())
        );
        results.insert(
            results.end(),
            std::make_move_iterator(ready.begin()),
            std::make_move_iterator(ready.end())
        );
        std::this_thread::yield();
    }
    return results;
}

shared::GenerationJob heightTileJob(
    uint64_t const id,
    shared::TerrainWorld const& world,
    std::shared_ptr<shared::RegionalArtifact const> leaf,
    shared::GenerationStage const stage = shared::GenerationStage::HeightTile,
    std::optional<uint64_t> const job_revision = std::nullopt
)
{
    return shared::GenerationJob{
        .id = id,
        .coordinate = { .x = 512, .y = 512, .z = 24 },
        .revision = job_revision.value_or(world.identity().revision),
        .seed = world.identity().seed,
        .stage = stage,
        .region = std::nullopt,
        .inherited_ancestor_data = nullptr,
        .terrain_identity = world.identity(),
        .world_epoch = 1U,
        .attempt = id,
        .inherited_region = std::move(leaf),
        .cancellation_source = std::make_shared<std::stop_source>(),
    };
}

shared::GenerationJob regionalJob(
    uint64_t const id,
    shared::TerrainWorld const& world,
    uint64_t const world_epoch,
    shared::GenerationStage const stage,
    uint32_t const extent,
    std::shared_ptr<shared::RegionalArtifact const> parent = nullptr
)
{
    auto const region = shared::generationRegionForChunk(
        {.x = 0, .y = 0, .z = 0},
        extent,
        stage,
        world.identity().revision,
        world.identity().seed
    );
    if (!region.has_value()) {
        throw std::runtime_error{"Expected a registered terrain region"};
    }
    return shared::GenerationJob{
        .id = id,
        .coordinate = {.x = 0, .y = 0, .z = 0},
        .revision = world.identity().revision,
        .seed = world.identity().seed,
        .stage = stage,
        .region = region,
        .inherited_ancestor_data = nullptr,
        .terrain_identity = world.identity(),
        .world_epoch = world_epoch,
        .attempt = id,
        .inherited_region = std::move(parent),
        .cancellation_source = std::make_shared<std::stop_source>(),
    };
}

std::shared_ptr<shared::RegionalArtifact const> preparedLeaf(
    shared::TerrainWorld const& world,
    shared::ChunkCoordinate const coordinate
)
{
    auto const region = shared::generationRegionForChunk(
        coordinate,
        shared::TERRAIN_REGION_EXTENTS.back(),
        shared::GenerationStage::Refinement,
        world.identity().revision,
        world.identity().seed
    );
    if (!region.has_value()) {
        throw std::runtime_error{"Expected a 64-block terrain region"};
    }
    return world.prepareRegion({
        .origin_x = static_cast<uint32_t>(region->origin_x),
        .origin_y = static_cast<uint32_t>(region->origin_y),
        .extent = region->extent,
    });
}

TEST(HeightTileWorkerPoolTest, SharesPreparedWorldAcrossPreviewAndMaterializationJobs)
{
    static constexpr uint32_t WORKER_COUNT = 1U;
    static constexpr core::ClientId CLIENT_ID = 7U;
    static constexpr uint64_t GENERATION = 4U;

    std::shared_ptr<shared::TerrainWorld const> const world = preparedTerrainWorld();
    server::HeightTileWorkerPool pool{world, WORKER_COUNT};
    std::shared_ptr<shared::RegionalArtifact const> const leaf = preparedLeaf(
        *world,
        {.x = 512, .y = 512, .z = 24}
    );
    ASSERT_TRUE(pool.enqueue({
        .client_id = CLIENT_ID,
        .generation = GENERATION,
        .job = heightTileJob(1U, *world, leaf, shared::GenerationStage::HeightTile, GENERATION),
    }));
    shared::GenerationJob materialization_job = heightTileJob(
        2U,
        *world,
        leaf,
        shared::GenerationStage::Materialize
    );
    materialization_job.coordinate.z = 25;
    ASSERT_TRUE(pool.enqueue({
        .client_id = CLIENT_ID,
        .generation = GENERATION,
        .job = materialization_job,
        .world_generation = true,
    }));

    std::vector<server::HeightTileWorkerPool::Result> results = takeResults(pool, 2U);
    ASSERT_EQ(results.size(), 2U);
    auto const preview = std::ranges::find_if(results, [](auto const& result) {
        return !result.world_generation;
    });
    auto const materialization = std::ranges::find_if(results, [](auto const& result) {
        return result.world_generation;
    });
    ASSERT_NE(preview, results.end());
    ASSERT_NE(materialization, results.end());
    ASSERT_TRUE(preview->succeeded);
    ASSERT_TRUE(materialization->succeeded);
    EXPECT_EQ(preview->tile.surface.identity, world->identity());
    EXPECT_EQ(materialization->tile.surface.identity, world->identity());
    EXPECT_EQ(preview->tile.heights, materialization->tile.heights);
    for (uint32_t index = 0U; index < shared::HeightTile::SAMPLE_COUNT; ++index) {
        EXPECT_EQ(preview->tile.heights[index],
            static_cast<uint16_t>(preview->tile.surface.columns[index].highest_solid_z + 1U));
        EXPECT_EQ(preview->tile.surface.columns[index].highest_solid_z,
            materialization->tile.surface.columns[index].highest_solid_z);
        EXPECT_EQ(preview->tile.surface.columns[index].top_material,
            materialization->tile.surface.columns[index].top_material);
    }
    shared::Chunk const expected_chunk = world->materializeChunk(
        materialization->tile.surface,
        materialization->job.coordinate
    );
    EXPECT_EQ(materialization->chunk.contentIdentity(), expected_chunk.contentIdentity());
    server::HeightTileWorkerPool::QueueMetrics const metrics = pool.queueMetrics();
    EXPECT_EQ(metrics.surface_tile_samples_total, 1U);
    EXPECT_EQ(metrics.surface_tile_cache_hits_total, 1U);
}

TEST(HeightTileWorkerPoolTest, DoesNotCacheCancelledOrStaleSurfaceWork)
{
    static constexpr uint32_t WORKER_COUNT = 1U;
    static constexpr core::ClientId CLIENT_ID = 7U;
    static constexpr uint64_t GENERATION = 4U;

    std::shared_ptr<shared::TerrainWorld const> const world = preparedTerrainWorld();
    server::HeightTileWorkerPool pool{world, WORKER_COUNT};
    std::shared_ptr<shared::RegionalArtifact const> const leaf = preparedLeaf(
        *world,
        {.x = 512, .y = 512, .z = 24}
    );
    shared::GenerationJob const cancelled_job = heightTileJob(
        1U,
        *world,
        leaf,
        shared::GenerationStage::HeightTile,
        GENERATION
    );
    cancelled_job.cancellation_source->request_stop();
    ASSERT_TRUE(pool.enqueue({
        .client_id = CLIENT_ID,
        .generation = GENERATION,
        .job = cancelled_job,
    }));
    auto cancelled_results = takeResults(pool, 1U);
    ASSERT_EQ(cancelled_results.size(), 1U);
    EXPECT_TRUE(cancelled_results.front().cancelled);
    EXPECT_EQ(pool.queueMetrics().surface_tile_samples_total, 0U);

    ASSERT_TRUE(pool.enqueue({
        .client_id = CLIENT_ID,
        .generation = GENERATION,
        .job = heightTileJob(2U, *world, leaf, shared::GenerationStage::Materialize),
        .world_generation = true,
    }));
    auto materialized = takeResults(pool, 1U);
    ASSERT_EQ(materialized.size(), 1U);
    ASSERT_TRUE(materialized.front().succeeded);
    EXPECT_EQ(pool.queueMetrics().surface_tile_samples_total, 1U);

    shared::GenerationJob stale_job = heightTileJob(
        3U,
        *world,
        leaf,
        shared::GenerationStage::HeightTile,
        GENERATION
    );
    ASSERT_TRUE(stale_job.terrain_identity.has_value());
    if (!stale_job.terrain_identity) {
        return;
    }
    ++stale_job.terrain_identity->revision;
    EXPECT_FALSE(pool.enqueue({
        .client_id = CLIENT_ID,
        .generation = GENERATION,
        .job = stale_job,
    }));
    EXPECT_EQ(pool.queueMetrics().surface_tile_cache_hits_total, 0U);

    ASSERT_TRUE(pool.enqueue({
        .client_id = CLIENT_ID,
        .generation = GENERATION,
        .job = heightTileJob(4U, *world, leaf, shared::GenerationStage::HeightTile, GENERATION),
    }));
    auto retried = takeResults(pool, 1U);
    ASSERT_EQ(retried.size(), 1U);
    ASSERT_TRUE(retried.front().succeeded);
    EXPECT_EQ(retried.front().tile.heights, materialized.front().tile.heights);
    server::HeightTileWorkerPool::QueueMetrics const metrics = pool.queueMetrics();
    EXPECT_EQ(metrics.surface_tile_samples_total, 1U);
    EXPECT_EQ(metrics.surface_tile_cache_hits_total, 1U);
}

TEST(HeightTileWorkerPoolTest, RejectsMissingGenerationContext)
{
    static constexpr uint32_t WORKER_COUNT = 1U;
    auto create_pool = [] {
        return std::make_unique<server::HeightTileWorkerPool>(
            std::shared_ptr<shared::TerrainWorld const>{},
            WORKER_COUNT
        );
    };

    EXPECT_THROW(static_cast<void>(create_pool()), std::invalid_argument);
}

TEST(HeightTileWorkerPoolTest, ExecutesTypedRefinementsOnlyWithTheExactParentLease)
{
    static constexpr uint32_t WORKER_COUNT = 1U;
    static constexpr uint64_t WORLD_EPOCH = 19U;
    static constexpr core::ClientId CLIENT_ID = 17U;
    std::shared_ptr<shared::TerrainWorld const> const world = preparedTerrainWorld();
    server::HeightTileWorkerPool pool{world, WORKER_COUNT, WORLD_EPOCH};

    shared::GenerationJob root = regionalJob(
        1U,
        *world,
        WORLD_EPOCH,
        shared::GenerationStage::Pregen,
        shared::TERRAIN_REGION_EXTENTS.front()
    );
    ASSERT_TRUE(pool.enqueue({
        .client_id = CLIENT_ID,
        .generation = WORLD_EPOCH,
        .job = root,
        .world_generation = true,
    }));
    auto root_results = takeResults(pool, 1U);
    ASSERT_EQ(root_results.size(), 1U);
    ASSERT_TRUE(root_results.front().succeeded);
    std::shared_ptr<shared::RegionalArtifact const> const root_artifact =
        root_results.front().regional_artifact;
    ASSERT_NE(root_artifact, nullptr);
    EXPECT_EQ(root_artifact->key().extent, shared::TERRAIN_REGION_EXTENTS.front());
    EXPECT_EQ(root_artifact->parent(), nullptr);

    shared::GenerationJob child;
    std::shared_ptr<shared::RegionalArtifact const> parent = root_artifact;
    for (uint32_t index = 1U; index < shared::TERRAIN_REGION_EXTENTS.size(); ++index) {
        child = regionalJob(
            static_cast<uint64_t>(index) + 1U,
            *world,
            WORLD_EPOCH,
            shared::GenerationStage::Refinement,
            shared::TERRAIN_REGION_EXTENTS[index],
            parent
        );
        ASSERT_TRUE(pool.enqueue({
            .client_id = CLIENT_ID,
            .generation = WORLD_EPOCH,
            .job = child,
            .world_generation = true,
        }));
        auto child_results = takeResults(pool, 1U);
        ASSERT_EQ(child_results.size(), 1U);
        ASSERT_TRUE(child_results.front().succeeded);
        ASSERT_NE(child_results.front().regional_artifact, nullptr);
        EXPECT_EQ(child_results.front().regional_artifact->parent(), parent);
        parent = child_results.front().regional_artifact;
    }
    ASSERT_EQ(parent->key().extent, shared::TERRAIN_REGION_EXTENTS.back());

    shared::GenerationJob missing_parent = child;
    missing_parent.id = 6U;
    missing_parent.attempt = 6U;
    missing_parent.inherited_region.reset();
    missing_parent.cancellation_source = std::make_shared<std::stop_source>();
    EXPECT_FALSE(pool.enqueue({
        .client_id = CLIENT_ID,
        .generation = WORLD_EPOCH,
        .job = missing_parent,
        .world_generation = true,
    }));

    shared::GenerationJob stale_epoch = child;
    stale_epoch.id = 7U;
    stale_epoch.attempt = 7U;
    stale_epoch.world_epoch = WORLD_EPOCH + 1U;
    stale_epoch.cancellation_source = std::make_shared<std::stop_source>();
    EXPECT_FALSE(pool.enqueue({
        .client_id = CLIENT_ID,
        .generation = WORLD_EPOCH,
        .job = stale_epoch,
        .world_generation = true,
    }));
}

TEST(HeightTileWorkerPoolTest, HonorsCancellationOnTypedRegionalJobs)
{
    static constexpr uint32_t WORKER_COUNT = 1U;
    static constexpr uint64_t WORLD_EPOCH = 23U;
    static constexpr core::ClientId CLIENT_ID = 17U;
    std::shared_ptr<shared::TerrainWorld const> const world = preparedTerrainWorld();
    server::HeightTileWorkerPool pool{world, WORKER_COUNT, WORLD_EPOCH};
    shared::GenerationJob job = regionalJob(
        1U,
        *world,
        WORLD_EPOCH,
        shared::GenerationStage::Pregen,
        shared::TERRAIN_REGION_EXTENTS.front()
    );
    job.cancellation_source->request_stop();

    ASSERT_TRUE(pool.enqueue({
        .client_id = CLIENT_ID,
        .generation = WORLD_EPOCH,
        .job = job,
        .world_generation = true,
    }));
    auto const results = takeResults(pool, 1U);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_TRUE(results.front().cancelled);
    EXPECT_FALSE(results.front().succeeded);
    EXPECT_EQ(results.front().regional_artifact, nullptr);
}

TEST(HeightTileWorkerPoolTest, BoundsOutstandingWorkAt128)
{
    static constexpr uint32_t WORKER_COUNT = 1U;
    static constexpr core::ClientId CLIENT_ID = 3U;
    static constexpr uint64_t GENERATION = 9U;
    static constexpr uint32_t MAX_OUTSTANDING_WORK = server::HeightTileWorkerPool::MAX_OUTSTANDING_WORK;

    std::shared_ptr<shared::TerrainWorld const> const world = preparedTerrainWorld();
    server::HeightTileWorkerPool pool{world, WORKER_COUNT};
    std::shared_ptr<shared::RegionalArtifact const> const leaf = preparedLeaf(
        *world,
        {.x = 512, .y = 512, .z = 24}
    );
    for (uint32_t index = 0U; index < MAX_OUTSTANDING_WORK; ++index) {
        EXPECT_TRUE(pool.enqueue({
            .client_id = CLIENT_ID,
            .generation = GENERATION,
            .job = heightTileJob(
                static_cast<uint64_t>(index) + 1U,
                *world,
                leaf,
                shared::GenerationStage::HeightTile,
                GENERATION
            ),
        }));
    }
    EXPECT_FALSE(pool.canAccept());
    EXPECT_FALSE(pool.enqueue({
        .client_id = CLIENT_ID,
        .generation = GENERATION,
        .job = heightTileJob(
            static_cast<uint64_t>(MAX_OUTSTANDING_WORK) + 1U,
            *world,
            leaf,
            shared::GenerationStage::HeightTile,
            GENERATION
        ),
    }));

    std::vector<server::HeightTileWorkerPool::Result> const results = takeResults(pool, MAX_OUTSTANDING_WORK);
    ASSERT_EQ(results.size(), MAX_OUTSTANDING_WORK);
    EXPECT_TRUE(std::ranges::all_of(results, [](auto const& result) { return result.succeeded; }));
    EXPECT_TRUE(pool.canAccept());
}

TEST(HeightTileWorkerPoolTest, CancellationDrainsSubmittedCompletionsAndQueuedWork)
{
    static constexpr uint32_t WORKER_COUNT = 1U;
    static constexpr core::ClientId CLIENT_ID = 11U;
    static constexpr uint64_t GENERATION = 22U;
    static constexpr uint32_t MAX_OUTSTANDING_WORK = server::HeightTileWorkerPool::MAX_OUTSTANDING_WORK;

    std::shared_ptr<shared::TerrainWorld const> const world = preparedTerrainWorld();
    server::HeightTileWorkerPool pool{world, WORKER_COUNT};
    std::shared_ptr<shared::RegionalArtifact const> const leaf = preparedLeaf(
        *world,
        {.x = 512, .y = 512, .z = 24}
    );
    std::atomic_bool hook_entered{false};
    std::atomic_bool release_hook{false};
    pool.setBeforeHeightTileCompletionHook([&hook_entered, &release_hook](core::ClientId, shared::HeightTileKey) {
        hook_entered.store(true, std::memory_order_release);
        while (!release_hook.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    EXPECT_TRUE(pool.enqueue({
        .client_id = CLIENT_ID,
        .generation = GENERATION,
        .job = heightTileJob(1U, *world, leaf, shared::GenerationStage::HeightTile, GENERATION),
    }));
    auto const hook_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!hook_entered.load(std::memory_order_acquire)
        && std::chrono::steady_clock::now() < hook_deadline) {
        std::this_thread::yield();
    }

    for (uint32_t index = 1U; index < MAX_OUTSTANDING_WORK; ++index) {
        EXPECT_TRUE(pool.enqueue({
            .client_id = CLIENT_ID,
            .generation = GENERATION,
            .job = heightTileJob(
                static_cast<uint64_t>(index) + 1U,
                *world,
                leaf,
                shared::GenerationStage::HeightTile,
                GENERATION
            ),
        }));
    }
    server::HeightTileWorkerPool::QueueMetrics const before_cancel = pool.queueMetrics();
    uint32_t const submitted_count = before_cancel.height_tiles.executor_queued_jobs
        + before_cancel.height_tiles.running_jobs
        + before_cancel.height_tiles.completed_uncollected_jobs;
    std::vector<shared::GenerationJob> const cancelled_queued = pool.cancelQueuedIf(
        CLIENT_ID,
        [](server::HeightTileWorkerPool::Work const&) { return true; }
    );
    pool.cancel(CLIENT_ID, GENERATION);
    release_hook.store(true, std::memory_order_release);

    std::vector<server::HeightTileWorkerPool::Result> const results = takeResults(pool, submitted_count);
    EXPECT_TRUE(hook_entered.load(std::memory_order_acquire));
    EXPECT_GT(cancelled_queued.size(), 0U);
    EXPECT_EQ(cancelled_queued.size(), before_cancel.height_tiles.pending_jobs);
    ASSERT_EQ(results.size(), submitted_count);
    EXPECT_TRUE(std::ranges::all_of(results, [](auto const& result) {
        return result.cancelled || result.succeeded;
    }));
    EXPECT_EQ(pool.queueMetrics().height_tiles.pending_jobs, 0U);
    EXPECT_TRUE(pool.canAccept());
}

} // namespace
