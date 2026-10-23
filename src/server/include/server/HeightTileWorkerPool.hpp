#pragma once

#include <shared/net/Message.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>
#include <core/net/Event.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace server {

struct HeightTileWorkerMetrics final {
    uint32_t pending_jobs = 0U;
    uint32_t executor_queued_jobs = 0U;
    uint32_t running_jobs = 0U;
    uint32_t completed_uncollected_jobs = 0U;
    uint64_t enqueued_total = 0U;
    uint64_t submitted_total = 0U;
    uint64_t started_total = 0U;
    uint64_t finished_total = 0U;
    uint64_t collected_total = 0U;
    std::chrono::nanoseconds enqueue_to_submit_total{};
    std::chrono::nanoseconds enqueue_to_submit_max{};
    std::chrono::nanoseconds enqueue_to_start_total{};
    std::chrono::nanoseconds enqueue_to_start_max{};
    std::chrono::nanoseconds executor_queue_total{};
    std::chrono::nanoseconds executor_queue_max{};
    std::chrono::nanoseconds execution_total{};
    std::chrono::nanoseconds execution_max{};
    std::chrono::nanoseconds completion_to_collection_total{};
    std::chrono::nanoseconds completion_to_collection_max{};
};

class HeightTileWorkerPool final {
public:
    static constexpr uint32_t MAX_OUTSTANDING_WORK = 128U;
    // Four 352-block interests touch at most 46 tile positions per axis (~83 MiB payload at 10.3 KiB/tile).
    static constexpr uint32_t MAX_CACHED_SURFACE_TILES = 4U * 46U * 46U;

    struct Work final {
        core::ClientId client_id;
        uint64_t generation;
        shared::GenerationJob job;
        bool world_generation = false;
    };

    struct Result final {
        core::ClientId client_id;
        uint64_t generation;
        shared::GenerationJob job;
        bool world_generation = false;
        bool succeeded = false;
        bool cancelled = false;
        shared::HeightTile tile{};
        shared::Chunk chunk{};
        std::shared_ptr<shared::RegionalArtifact const> regional_artifact = nullptr;
        std::vector<uint8_t> generation_output;
    };

    struct QueueMetrics final {
        HeightTileWorkerMetrics height_tiles;
        HeightTileWorkerMetrics world_generation;
        uint64_t surface_tile_cache_hits_total = 0U;
        uint64_t surface_tile_samples_total = 0U;
    };

    HeightTileWorkerPool(
        std::shared_ptr<shared::TerrainWorld const> terrain_world,
        uint32_t worker_count,
        uint64_t world_epoch = 1U
    );
    HeightTileWorkerPool(
        std::shared_ptr<shared::TerrainGenerator const> legacy_generator,
        uint32_t worker_count
    );
    ~HeightTileWorkerPool();

    HeightTileWorkerPool(HeightTileWorkerPool const&) = delete;
    HeightTileWorkerPool& operator=(HeightTileWorkerPool const&) = delete;
    HeightTileWorkerPool(HeightTileWorkerPool&&) = delete;
    HeightTileWorkerPool& operator=(HeightTileWorkerPool&&) = delete;

    [[nodiscard]]
    bool canAccept() const;

    [[nodiscard]]
    QueueMetrics queueMetrics() const;

    void setBenchmarkMetricsEnabled(bool enabled);

    void setBeforeHeightTileCompletionHook(
        std::function<void(core::ClientId, shared::HeightTileKey)> hook
    );

    [[nodiscard]]
    bool enqueue(Work work);

    void cancel(core::ClientId client_id, uint64_t generation);

    [[nodiscard]]
    std::vector<shared::GenerationJob> cancelQueued(core::ClientId client_id, uint64_t generation);

    [[nodiscard]]
    std::vector<shared::GenerationJob> cancelQueuedIf(
        core::ClientId client_id,
        std::function<bool(Work const&)> const& should_cancel
    );

    void reorderQueued(
        core::ClientId client_id,
        std::function<bool(Work const&, Work const&)> const& order
    );

    [[nodiscard]]
    std::vector<Result> takeResults(uint32_t maximum_results);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace server
