#include <server/HeightTileWorkerPool.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>
#include <generation/Generation.hpp>
#include <core/executor/Executor.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct SurfaceTileCacheKey final {
    shared::TerrainIdentity terrain_identity;
    uint64_t world_epoch = 0U;
    shared::TerrainRegionKey leaf;
    shared::HeightTileCoordinate coordinate;

    bool operator==(SurfaceTileCacheKey const&) const = default;
};

struct SurfaceTileCacheKeyHash final {
    [[nodiscard]]
    size_t operator()(SurfaceTileCacheKey const& key) const noexcept
    {
        size_t seed = 0U;
        auto combine = [&seed](size_t const value) {
            seed ^= value + 0x9e37'79b9U + (seed << 6U) + (seed >> 2U);
        };
        combine(std::hash<uint32_t>{}(key.terrain_identity.terrain_version));
        combine(std::hash<uint64_t>{}(key.terrain_identity.revision));
        combine(std::hash<uint64_t>{}(key.terrain_identity.seed));
        combine(std::hash<std::string>{}(key.terrain_identity.configuration_identity));
        combine(std::hash<uint64_t>{}(key.world_epoch));
        combine(std::hash<uint32_t>{}(key.leaf.origin_x));
        combine(std::hash<uint32_t>{}(key.leaf.origin_y));
        combine(std::hash<uint32_t>{}(key.leaf.extent));
        combine(std::hash<int32_t>{}(key.coordinate.x));
        combine(std::hash<int32_t>{}(key.coordinate.y));
        return seed;
    }
};

class SurfaceTileCache final {
public:
    using Tile = std::shared_ptr<shared::SurfaceTile const>;
    static constexpr size_t CAPACITY = server::HeightTileWorkerPool::MAX_CACHED_SURFACE_TILES;

    [[nodiscard]]
    Tile getOrSample(
        shared::TerrainWorld const& terrain_world,
        shared::RegionalArtifact const& leaf,
        uint64_t const world_epoch,
        shared::HeightTileCoordinate const coordinate,
        std::stop_token const& cancellation
    )
    {
        if (cancellation.stop_requested()) {
            throw generation::GenerationCancelled{};
        }
        if (world_epoch == 0U || leaf.identity() != terrain_world.identity()
            || leaf.key().extent != shared::TERRAIN_REGION_EXTENTS.back()) {
            throw std::logic_error{"surface tile cache requires a matching typed leaf"};
        }

        SurfaceTileCacheKey key{
            .terrain_identity = terrain_world.identity(),
            .world_epoch = world_epoch,
            .leaf = leaf.key(),
            .coordinate = coordinate,
        };
        {
            std::lock_guard lock{m_mutex};
            auto const found = m_entries.find(key);
            if (found != m_entries.end()) {
                if (cancellation.stop_requested()) {
                    throw generation::GenerationCancelled{};
                }
                m_recency.splice(m_recency.begin(), m_recency, found->second.recency);
                m_hits.fetch_add(1U, std::memory_order_relaxed);
                return found->second.tile;
            }
        }

        shared::SurfaceTile sampled = terrain_world.sampleSurface(
            leaf,
            {
                .x = coordinate.x,
                .y = coordinate.y,
                .z = 0,
            },
            cancellation
        );
        m_samples.fetch_add(1U, std::memory_order_relaxed);
        if (cancellation.stop_requested()) {
            throw generation::GenerationCancelled{};
        }
        Tile tile = std::make_shared<shared::SurfaceTile const>(std::move(sampled));
        {
            std::lock_guard lock{m_mutex};
            // Stop can arrive during sampling, so poll again before publishing the result.
            // cppcheck-suppress identicalConditionAfterEarlyExit
            if (cancellation.stop_requested()) {
                throw generation::GenerationCancelled{};
            }
            auto const existing = m_entries.find(key);
            if (existing != m_entries.end()) {
                m_recency.splice(m_recency.begin(), m_recency, existing->second.recency);
                return existing->second.tile;
            }
            if (m_entries.size() == CAPACITY) {
                m_entries.erase(m_recency.back());
                m_recency.pop_back();
            }
            m_recency.push_front(key);
            m_entries.emplace(std::move(key), Entry{tile, m_recency.begin()});
        }
        return tile;
    }

    [[nodiscard]]
    uint64_t cacheHits() const noexcept
    {
        return m_hits.load(std::memory_order_relaxed);
    }

    [[nodiscard]]
    uint64_t completedSamples() const noexcept
    {
        return m_samples.load(std::memory_order_relaxed);
    }

private:
    struct Entry final {
        Tile tile;
        std::list<SurfaceTileCacheKey>::iterator recency;
    };

    mutable std::mutex m_mutex;
    std::list<SurfaceTileCacheKey> m_recency;
    std::unordered_map<SurfaceTileCacheKey, Entry, SurfaceTileCacheKeyHash> m_entries;
    std::atomic<uint64_t> m_hits{0U};
    std::atomic<uint64_t> m_samples{0U};
};

shared::HeightTile sampleHeightTile(
    shared::TerrainWorld const& terrain_world,
    std::shared_ptr<shared::RegionalArtifact const> const& leaf,
    SurfaceTileCache& surface_tile_cache,
    uint64_t const world_epoch,
    shared::HeightTileCoordinate const coordinate,
    std::stop_token const& cancellation
)
{
    shared::HeightTile tile;
    tile.coordinate = coordinate;
    tile.surface = *surface_tile_cache.getOrSample(
        terrain_world,
        *leaf,
        world_epoch,
        {
            .x = coordinate.x,
            .y = coordinate.y,
        },
        cancellation
    );
    for (uint32_t index = 0U; index < shared::HeightTile::SAMPLE_COUNT; ++index) {
        if (cancellation.stop_requested()) {
            throw generation::GenerationCancelled{};
        }
        tile.heights[index] = static_cast<uint16_t>(tile.surface.columns[index].highest_solid_z + 1U);
    }
    return tile;
}

[[nodiscard]]
bool matchesTypedContext(
    shared::GenerationJob const& job,
    shared::TerrainWorld const& terrain_world,
    uint64_t const world_epoch,
    bool const world_generation
) noexcept
{
    return world_epoch != 0U
        && job.world_epoch == world_epoch
        && job.terrain_identity.has_value()
        && *job.terrain_identity == terrain_world.identity()
        && job.seed == terrain_world.identity().seed
        && (!world_generation || job.revision == terrain_world.identity().revision);
}

[[nodiscard]]
bool matchesLeaf(
    std::shared_ptr<shared::RegionalArtifact const> const& leaf,
    shared::GenerationJob const& job,
    shared::TerrainWorld const& terrain_world
) noexcept
{
    if (leaf == nullptr || !job.terrain_identity.has_value()
        || *job.terrain_identity != terrain_world.identity()
        || leaf->identity() != terrain_world.identity()
        || leaf->key().extent != shared::TERRAIN_REGION_EXTENTS.back()) {
        return false;
    }
    shared::TerrainIdentity const& identity = *job.terrain_identity;
    auto const expected = shared::generationRegionForChunk(
        job.coordinate,
        shared::TERRAIN_REGION_EXTENTS.back(),
        shared::GenerationStage::Refinement,
        identity.revision,
        identity.seed
    );
    return expected.has_value()
        && static_cast<int32_t>(leaf->key().origin_x) == expected->origin_x
        && static_cast<int32_t>(leaf->key().origin_y) == expected->origin_y;
}

[[nodiscard]]
bool hasExactRegionalParent(shared::GenerationJob const& job, shared::TerrainWorld const& terrain_world) noexcept
{
    if (!job.region.has_value()) {
        return false;
    }
    shared::GenerationRegionKey const& region = *job.region;
    auto const extent = std::ranges::find(shared::TERRAIN_REGION_EXTENTS, region.extent);
    auto const expected_region = shared::generationRegionForChunk(
        job.coordinate,
        region.extent,
        job.stage,
        terrain_world.identity().revision,
        terrain_world.identity().seed
    );
    if (extent == shared::TERRAIN_REGION_EXTENTS.end()
        || !expected_region.has_value() || *expected_region != region
        || region.revision != terrain_world.identity().revision
        || region.seed != terrain_world.identity().seed
        || region.origin_x < 0 || region.origin_y < 0) {
        return false;
    }
    if (extent == shared::TERRAIN_REGION_EXTENTS.begin()) {
        return job.stage == shared::GenerationStage::Pregen
            && job.inherited_region == nullptr
            && region.extent == shared::TERRAIN_REGION_EXTENTS.front();
    }
    if (job.stage != shared::GenerationStage::Refinement || job.inherited_region == nullptr) {
        return false;
    }
    uint32_t const parent_extent = *std::prev(extent);
    shared::RegionalArtifact const& parent = *job.inherited_region;
    return parent.identity() == terrain_world.identity()
        && parent.key().extent == parent_extent
        && parent.key().origin_x
            == (static_cast<uint32_t>(region.origin_x) / parent_extent) * parent_extent
        && parent.key().origin_y
            == (static_cast<uint32_t>(region.origin_y) / parent_extent) * parent_extent;
}

[[nodiscard]]
std::vector<uint8_t> generateRegionOutput(
    shared::GenerationJob const& job,
    shared::TerrainGenerator const& terrain_generator
)
{
    if (!job.region.has_value()) {
        throw std::runtime_error{"world refinement job is missing its bounded region"};
    }
    static constexpr uint32_t SAMPLE_SIDE = 16U;
    static constexpr uint32_t SAMPLE_COUNT = SAMPLE_SIDE * SAMPLE_SIDE;
    static constexpr uint32_t SAMPLE_BYTES = sizeof(uint16_t);
    static constexpr uint32_t REGION_HEADER_BYTES = 3U * sizeof(uint32_t);
    static constexpr uint32_t REGION_OUTPUT_BYTES = REGION_HEADER_BYTES + SAMPLE_COUNT * SAMPLE_BYTES;
    static constexpr uint64_t SAMPLE_DIAMETER = static_cast<uint64_t>(SAMPLE_SIDE) * 2U;
    if (job.inherited_ancestor_data != nullptr
        && job.inherited_ancestor_data->size() != REGION_OUTPUT_BYTES) {
        throw std::runtime_error{"world refinement ancestor has an invalid sample payload"};
    }
    std::vector<uint8_t> output(REGION_OUTPUT_BYTES);
    auto write_header = [&output](uint32_t const offset, uint32_t const value) {
        for (uint32_t byte = 0U; byte < sizeof(uint32_t); ++byte) {
            output[offset + byte] = static_cast<uint8_t>(value >> (byte * 8U));
        }
    };
    write_header(0U, static_cast<uint32_t>(job.region->origin_x));
    write_header(sizeof(uint32_t), static_cast<uint32_t>(job.region->origin_y));
    write_header(2U * sizeof(uint32_t), job.region->extent);
    for (uint32_t y = 0U; y < SAMPLE_SIDE; ++y) {
        for (uint32_t x = 0U; x < SAMPLE_SIDE; ++x) {
            uint64_t const sample_x = static_cast<uint64_t>(2U * x + 1U)
                * job.region->extent / SAMPLE_DIAMETER;
            uint64_t const sample_y = static_cast<uint64_t>(2U * y + 1U)
                * job.region->extent / SAMPLE_DIAMETER;
            uint16_t height = terrain_generator.heightAt(
                static_cast<int64_t>(job.region->origin_x) + static_cast<int64_t>(sample_x),
                static_cast<int64_t>(job.region->origin_y) + static_cast<int64_t>(sample_y)
            );
            uint32_t const sample_index = y * SAMPLE_SIDE + x;
            if (job.inherited_ancestor_data != nullptr) {
                auto const& ancestor = *job.inherited_ancestor_data;
                auto read_header = [&ancestor](uint32_t const offset) {
                    uint32_t value = 0U;
                    for (uint32_t byte = 0U; byte < sizeof(uint32_t); ++byte) {
                        value |= static_cast<uint32_t>(ancestor[offset + byte]) << (byte * 8U);
                    }
                    return value;
                };
                uint32_t const ancestor_origin_x = read_header(0U);
                uint32_t const ancestor_origin_y = read_header(sizeof(uint32_t));
                uint32_t const ancestor_extent = read_header(2U * sizeof(uint32_t));
                if (ancestor_extent <= job.region->extent
                    || ancestor_origin_x > static_cast<uint32_t>(job.region->origin_x)
                    || ancestor_origin_y > static_cast<uint32_t>(job.region->origin_y)
                    || static_cast<uint64_t>(ancestor_origin_x) + ancestor_extent
                        < static_cast<uint64_t>(job.region->origin_x) + job.region->extent
                    || static_cast<uint64_t>(ancestor_origin_y) + ancestor_extent
                        < static_cast<uint64_t>(job.region->origin_y) + job.region->extent) {
                    throw std::runtime_error{"world refinement ancestor does not contain its child"};
                }
                uint64_t const offset_x = static_cast<uint64_t>(job.region->origin_x)
                    + sample_x - ancestor_origin_x;
                uint64_t const offset_y = static_cast<uint64_t>(job.region->origin_y)
                    + sample_y - ancestor_origin_y;
                uint32_t const ancestor_x = std::min<uint32_t>(
                    SAMPLE_SIDE - 1U,
                    static_cast<uint32_t>(offset_x * SAMPLE_SIDE / ancestor_extent)
                );
                uint32_t const ancestor_y = std::min<uint32_t>(
                    SAMPLE_SIDE - 1U,
                    static_cast<uint32_t>(offset_y * SAMPLE_SIDE / ancestor_extent)
                );
                uint32_t const ancestor_index = ancestor_y * SAMPLE_SIDE + ancestor_x;
                uint16_t const inherited_height = static_cast<uint16_t>(ancestor[
                    REGION_HEADER_BYTES + ancestor_index * SAMPLE_BYTES
                ]) | static_cast<uint16_t>(static_cast<uint16_t>(ancestor[
                    REGION_HEADER_BYTES + ancestor_index * SAMPLE_BYTES + 1U
                ]) << 8U);
                height = static_cast<uint16_t>((static_cast<uint32_t>(height) + inherited_height) / 2U);
            }
            output[REGION_HEADER_BYTES + sample_index * SAMPLE_BYTES] = static_cast<uint8_t>(height);
            output[REGION_HEADER_BYTES + sample_index * SAMPLE_BYTES + 1U] = static_cast<uint8_t>(height >> 8U);
        }
    }
    return output;
}

} // namespace

namespace server {

struct HeightTileWorkerPool::Impl final {
    static constexpr uint32_t MAX_OUTSTANDING_WORK = HeightTileWorkerPool::MAX_OUTSTANDING_WORK;

    enum class WorkState : uint8_t {
        ExecutorQueued,
        Running,
        Finished,
    };

    struct JobTiming final {
        std::atomic<WorkState> state{WorkState::ExecutorQueued};
        std::chrono::steady_clock::time_point enqueued_at{};
        std::chrono::steady_clock::time_point submitted_at{};
        std::chrono::steady_clock::time_point started_at{};
        std::chrono::steady_clock::time_point finished_at{};
    };

    struct AtomicJobMetrics final {
        std::atomic<uint64_t> enqueued{};
        std::atomic<uint64_t> submitted{};
        std::atomic<uint64_t> started{};
        std::atomic<uint64_t> finished{};
        std::atomic<uint64_t> collected{};
        std::atomic<uint64_t> enqueue_to_submit_total_ns{};
        std::atomic<uint64_t> enqueue_to_submit_max_ns{};
        std::atomic<uint64_t> enqueue_to_start_total_ns{};
        std::atomic<uint64_t> enqueue_to_start_max_ns{};
        std::atomic<uint64_t> executor_queue_total_ns{};
        std::atomic<uint64_t> executor_queue_max_ns{};
        std::atomic<uint64_t> execution_total_ns{};
        std::atomic<uint64_t> execution_max_ns{};
        std::atomic<uint64_t> completion_to_collection_total_ns{};
        std::atomic<uint64_t> completion_to_collection_max_ns{};
    };

    struct DurationCounters final {
        std::atomic<uint64_t>& total;
        std::atomic<uint64_t>& maximum;
    };

    using Work = HeightTileWorkerPool::Work;
    using Result = HeightTileWorkerPool::Result;

    struct QueuedWork final {
        Work work;
        std::shared_ptr<JobTiming> timing;
    };

    struct Submitted final {
        Work work;
        std::shared_ptr<Result> result;
        std::shared_ptr<JobTiming> timing;
        core::executor::JobHandle handle;
    };

    using QueueMetrics = HeightTileWorkerPool::QueueMetrics;

    struct Configuration final {
        uint32_t worker_count;
        uint64_t world_epoch;
    };

    Impl(
        std::shared_ptr<shared::TerrainWorld const> terrain_world,
        std::shared_ptr<shared::TerrainGenerator const> legacy_generator,
        Configuration const configuration
    )
        : m_terrain_world{std::move(terrain_world)}
        , m_terrain_generator{std::move(legacy_generator)}
        , m_world_epoch{configuration.world_epoch}
    {
        if ((m_terrain_world == nullptr) == (m_terrain_generator == nullptr)) {
            throw std::invalid_argument{ "terrain worker requires exactly one explicit generation context" };
        }
        if (configuration.worker_count == 0U) {
            throw std::invalid_argument{ "terrain worker count must be positive" };
        }
        if (m_terrain_world != nullptr && m_world_epoch == 0U) {
            throw std::invalid_argument{"typed terrain worker requires a nonzero world epoch"};
        }
        // The admission bound keeps submitted work bounded while sustaining workers between main-loop refills.
        m_submission_window = MAX_OUTSTANDING_WORK;
        m_executor = std::make_unique<core::executor::Executor>(
            core::executor::ExecutorLimits{
                .queue_capacity = configuration.worker_count,
                .result_capacity = MAX_OUTSTANDING_WORK,
            },
            configuration.worker_count
        );
    }

    ~Impl()
    {
        m_executor->shutdown();
    }

    [[nodiscard]]
    bool canAccept() const
    {
        std::lock_guard lock{m_mutex};
        return outstandingCount() < MAX_OUTSTANDING_WORK;
    }

    [[nodiscard]]
    QueueMetrics queueMetrics() const
    {
        std::lock_guard lock{m_mutex};
        QueueMetrics metrics;
        for (QueuedWork const& queued : m_pending) {
            ++metricsFor(metrics, queued.work.world_generation).pending_jobs;
        }
        for (auto const& [id, submitted] : m_submitted) {
            static_cast<void>(id);
            HeightTileWorkerMetrics& job_metrics = metricsFor(metrics, submitted.work.world_generation);
            ++job_metrics.submitted_total;
            if (submitted.timing == nullptr) {
                ++job_metrics.executor_queued_jobs;
                continue;
            }
            switch (submitted.timing->state.load(std::memory_order_acquire)) {
                case WorkState::ExecutorQueued:
                    ++job_metrics.executor_queued_jobs;
                    break;
                case WorkState::Running:
                    ++job_metrics.running_jobs;
                    break;
                case WorkState::Finished:
                    ++job_metrics.completed_uncollected_jobs;
                    break;
            }
        }
        copyAtomicMetrics(m_height_tile_metrics, metrics.height_tiles);
        copyAtomicMetrics(m_world_generation_metrics, metrics.world_generation);
        metrics.surface_tile_cache_hits_total = m_surface_tile_cache.cacheHits();
        metrics.surface_tile_samples_total = m_surface_tile_cache.completedSamples();
        return metrics;
    }

    void setBenchmarkMetricsEnabled(bool const enabled)
    {
        std::lock_guard lock{m_mutex};
        m_benchmark_metrics_enabled = enabled;
    }

    void setBeforeHeightTileCompletionHook(
        std::function<void(core::ClientId, shared::HeightTileKey)> hook
    )
    {
        std::lock_guard lock{m_mutex};
        m_before_height_tile_completion = std::move(hook);
    }

    [[nodiscard]]
    bool enqueue(Work work)
    {
        std::lock_guard lock{m_mutex};
        if (outstandingCount() >= MAX_OUTSTANDING_WORK || !validWork(work)) {
            return false;
        }
        std::shared_ptr<JobTiming> timing;
        if (m_benchmark_metrics_enabled) {
            timing = std::make_shared<JobTiming>();
            timing->enqueued_at = std::chrono::steady_clock::now();
            atomicMetrics(work.world_generation).enqueued.fetch_add(1U, std::memory_order_relaxed);
        }
        m_pending.push_back({.work = std::move(work), .timing = std::move(timing)});
        submitPending();
        return true;
    }

    void cancel(core::ClientId const client_id, uint64_t const generation)
    {
        std::lock_guard lock{m_mutex};
        std::erase_if(m_pending, [client_id, generation](QueuedWork const& queued) {
            Work const& work = queued.work;
            if (work.world_generation || work.client_id != client_id || work.generation != generation) {
                return false;
            }
            if (work.job.cancellation_source != nullptr) {
                work.job.cancellation_source->request_stop();
            }
            return true;
        });
        for (auto& [id, submitted] : m_submitted) {
            static_cast<void>(id);
            if (!submitted.work.world_generation && submitted.work.client_id == client_id
                && submitted.work.generation == generation) {
                if (submitted.work.job.cancellation_source != nullptr) {
                    submitted.work.job.cancellation_source->request_stop();
                }
                static_cast<void>(submitted.handle.requestCancellation());
            }
        }
    }

    [[nodiscard]]
    std::vector<shared::GenerationJob> cancelQueued(
        core::ClientId const client_id,
        uint64_t const generation
    )
    {
        std::vector<shared::GenerationJob> cancelled;
        std::lock_guard lock{m_mutex};
        std::erase_if(m_pending, [&](QueuedWork const& queued) {
            Work const& work = queued.work;
            if (work.world_generation || work.client_id != client_id || work.generation != generation) {
                return false;
            }
            if (work.job.cancellation_source != nullptr) {
                work.job.cancellation_source->request_stop();
            }
            cancelled.push_back(work.job);
            return true;
        });
        return cancelled;
    }

    [[nodiscard]]
    std::vector<shared::GenerationJob> cancelQueuedIf(
        core::ClientId const client_id,
        std::function<bool(Work const&)> const& should_cancel
    )
    {
        std::vector<shared::GenerationJob> cancelled;
        std::lock_guard lock{m_mutex};
        std::erase_if(m_pending, [&](QueuedWork const& queued) {
            Work const& work = queued.work;
            if (work.world_generation || work.client_id != client_id || !should_cancel(work)) {
                return false;
            }
            if (work.job.cancellation_source != nullptr) {
                work.job.cancellation_source->request_stop();
            }
            cancelled.push_back(work.job);
            return true;
        });
        for (auto& [id, submitted] : m_submitted) {
            static_cast<void>(id);
            if (!submitted.work.world_generation && submitted.work.client_id == client_id
                && should_cancel(submitted.work)) {
                if (submitted.work.job.cancellation_source != nullptr) {
                    submitted.work.job.cancellation_source->request_stop();
                }
                static_cast<void>(submitted.handle.requestCancellation());
            }
        }
        return cancelled;
    }

    void reorderQueued(
        core::ClientId const client_id,
        std::function<bool(Work const&, Work const&)> const& order
    )
    {
        std::lock_guard lock{m_mutex};
        std::vector<QueuedWork> client_pending;
        client_pending.reserve(m_pending.size());
        for (QueuedWork const& queued : m_pending) {
            Work const& work = queued.work;
            if (!work.world_generation && work.client_id == client_id) {
                client_pending.push_back(queued);
            }
        }
        std::ranges::stable_sort(client_pending, [&order](QueuedWork const& first, QueuedWork const& second) {
            return order(first.work, second.work);
        });
        auto reordered = client_pending.begin();
        for (QueuedWork& queued : m_pending) {
            if (!queued.work.world_generation && queued.work.client_id == client_id) {
                queued = std::move(*reordered);
                ++reordered;
            }
        }
    }

    [[nodiscard]]
    std::vector<Result> takeResults(uint32_t const maximum_results)
    {
        std::vector<Result> results;
        std::lock_guard lock{m_mutex};
        results.reserve(std::min<uint32_t>(
            maximum_results,
            static_cast<uint32_t>(m_submitted.size())
        ));
        while (static_cast<uint32_t>(results.size()) < maximum_results) {
            std::optional<core::executor::JobResult> const completion = m_executor->tryTakeResult();
            if (!completion.has_value()) {
                break;
            }
            auto const submitted = m_submitted.find(completion->id());
            if (submitted == m_submitted.end()) {
                continue;
            }
            std::shared_ptr<JobTiming> const timing = submitted->second.timing;
            if (timing != nullptr) {
                AtomicJobMetrics& job_metrics = atomicMetrics(submitted->second.work.world_generation);
                WorkState const state = timing->state.load(std::memory_order_acquire);
                if (state == WorkState::Finished) {
                    auto const collection_delay = std::chrono::steady_clock::now() - timing->finished_at;
                    addDuration(
                        {
                            .total = job_metrics.completion_to_collection_total_ns,
                            .maximum = job_metrics.completion_to_collection_max_ns,
                        },
                        collection_delay
                    );
                }
                job_metrics.collected.fetch_add(1U, std::memory_order_relaxed);
            }
            submitted->second.result->cancelled = submitted->second.result->cancelled
                || completion->status() == core::executor::CompletionStatus::Cancelled;
            submitted->second.result->succeeded = !submitted->second.result->cancelled
                && completion->status() == core::executor::CompletionStatus::Succeeded;
            results.push_back(std::move(*submitted->second.result));
            m_submitted.erase(submitted);
        }
        submitPending();
        return results;
    }

private:
    [[nodiscard]]
    bool validWork(Work const& work) const noexcept
    {
        if (m_terrain_world == nullptr) {
            return true;
        }
        if (!matchesTypedContext(work.job, *m_terrain_world, m_world_epoch, work.world_generation)
            || work.job.attempt == 0U || work.job.cancellation_source == nullptr) {
            return false;
        }
        if (!work.world_generation && work.job.revision != work.generation) {
            return false;
        }
        if (work.job.stage == shared::GenerationStage::Pregen
            || work.job.stage == shared::GenerationStage::Refinement) {
            return work.world_generation && hasExactRegionalParent(work.job, *m_terrain_world);
        }
        if (work.job.stage == shared::GenerationStage::HeightTile) {
            return !work.job.region.has_value() && matchesLeaf(
                work.job.inherited_region,
                work.job,
                *m_terrain_world
            );
        }
        return work.job.stage == shared::GenerationStage::Materialize
            && work.world_generation
            && !work.job.region.has_value()
            && matchesLeaf(work.job.inherited_region, work.job, *m_terrain_world);
    }

    [[nodiscard]]
    static HeightTileWorkerMetrics& metricsFor(QueueMetrics& metrics, bool const world_generation) noexcept
    {
        return world_generation ? metrics.world_generation : metrics.height_tiles;
    }

    [[nodiscard]]
    AtomicJobMetrics& atomicMetrics(bool const world_generation) noexcept
    {
        return world_generation ? m_world_generation_metrics : m_height_tile_metrics;
    }

    [[nodiscard]]
    AtomicJobMetrics const& atomicMetrics(bool const world_generation) const noexcept
    {
        return world_generation ? m_world_generation_metrics : m_height_tile_metrics;
    }

    static void updateMaximum(std::atomic<uint64_t>& maximum, uint64_t const candidate) noexcept
    {
        uint64_t observed = maximum.load(std::memory_order_relaxed);
        while (observed < candidate) {
            if (maximum.compare_exchange_weak(
                    observed,
                    candidate,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed
                )) {
                return;
            }
        }
    }

    static void addDuration(
        DurationCounters const counters,
        std::chrono::steady_clock::duration const duration
    ) noexcept
    {
        auto const nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
        uint64_t const count = static_cast<uint64_t>(std::max<int64_t>(nanoseconds.count(), 0));
        counters.total.fetch_add(count, std::memory_order_relaxed);
        updateMaximum(counters.maximum, count);
    }

    static void copyAtomicMetrics(AtomicJobMetrics const& source, HeightTileWorkerMetrics& destination)
    {
        destination.enqueued_total = source.enqueued.load(std::memory_order_relaxed);
        destination.submitted_total = source.submitted.load(std::memory_order_relaxed);
        destination.started_total = source.started.load(std::memory_order_relaxed);
        destination.finished_total = source.finished.load(std::memory_order_relaxed);
        destination.collected_total = source.collected.load(std::memory_order_relaxed);
        destination.enqueue_to_submit_total = std::chrono::nanoseconds{
            source.enqueue_to_submit_total_ns.load(std::memory_order_relaxed),
        };
        destination.enqueue_to_submit_max = std::chrono::nanoseconds{
            source.enqueue_to_submit_max_ns.load(std::memory_order_relaxed),
        };
        destination.enqueue_to_start_total = std::chrono::nanoseconds{
            source.enqueue_to_start_total_ns.load(std::memory_order_relaxed),
        };
        destination.enqueue_to_start_max = std::chrono::nanoseconds{
            source.enqueue_to_start_max_ns.load(std::memory_order_relaxed),
        };
        destination.executor_queue_total = std::chrono::nanoseconds{
            source.executor_queue_total_ns.load(std::memory_order_relaxed),
        };
        destination.executor_queue_max = std::chrono::nanoseconds{
            source.executor_queue_max_ns.load(std::memory_order_relaxed),
        };
        destination.execution_total = std::chrono::nanoseconds{
            source.execution_total_ns.load(std::memory_order_relaxed),
        };
        destination.execution_max = std::chrono::nanoseconds{
            source.execution_max_ns.load(std::memory_order_relaxed),
        };
        destination.completion_to_collection_total = std::chrono::nanoseconds{
            source.completion_to_collection_total_ns.load(std::memory_order_relaxed),
        };
        destination.completion_to_collection_max = std::chrono::nanoseconds{
            source.completion_to_collection_max_ns.load(std::memory_order_relaxed),
        };
    }

    void recordStarted(
        Result const& result,
        std::shared_ptr<JobTiming> const& timing,
        std::chrono::steady_clock::time_point const started_at
    )
    {
        if (timing == nullptr) {
            return;
        }
        timing->started_at = started_at;
        timing->state.store(WorkState::Running, std::memory_order_release);
        AtomicJobMetrics& metrics = atomicMetrics(result.world_generation);
        metrics.started.fetch_add(1U, std::memory_order_relaxed);
        addDuration(
            {
                .total = metrics.enqueue_to_start_total_ns,
                .maximum = metrics.enqueue_to_start_max_ns,
            },
            started_at - timing->enqueued_at
        );
        addDuration(
            {
                .total = metrics.executor_queue_total_ns,
                .maximum = metrics.executor_queue_max_ns,
            },
            started_at - timing->submitted_at
        );
    }

    void recordFinished(
        Result const& result,
        std::shared_ptr<JobTiming> const& timing,
        std::chrono::steady_clock::time_point const finished_at
    )
    {
        if (timing == nullptr) {
            return;
        }
        timing->finished_at = finished_at;
        timing->state.store(WorkState::Finished, std::memory_order_release);
        AtomicJobMetrics& metrics = atomicMetrics(result.world_generation);
        metrics.finished.fetch_add(1U, std::memory_order_relaxed);
        addDuration(
            {
                .total = metrics.execution_total_ns,
                .maximum = metrics.execution_max_ns,
            },
            finished_at - timing->started_at
        );
    }

    [[nodiscard]] uint32_t outstandingCount() const noexcept
    {
        return static_cast<uint32_t>(m_pending.size() + m_submitted.size());
    }

    static void executeGenerationJob(
        Result& result,
        shared::TerrainGenerator const* generator,
        shared::TerrainWorld const* terrain_world,
        SurfaceTileCache& surface_tile_cache,
        uint64_t const world_epoch,
        std::stop_token const& cancellation
    )
    {
        if (terrain_world != nullptr) {
            if (!matchesTypedContext(result.job, *terrain_world, world_epoch, true)) {
                throw std::logic_error{"typed terrain worker received a mismatched world identity or epoch"};
            }
            if (result.job.stage == shared::GenerationStage::Pregen
                || result.job.stage == shared::GenerationStage::Refinement) {
                if (!result.job.region.has_value()
                    || !hasExactRegionalParent(result.job, *terrain_world)) {
                    throw std::logic_error{"typed regional job has no exact parent lease"};
                }
                shared::GenerationRegionKey const& region = result.job.region.value();
                shared::TerrainRegionKey const key{
                    .origin_x = static_cast<uint32_t>(region.origin_x),
                    .origin_y = static_cast<uint32_t>(region.origin_y),
                    .extent = region.extent,
                };
                result.regional_artifact = terrain_world->refineRegion(
                    key,
                    result.job.inherited_region,
                    cancellation
                );
                return;
            }
            if (result.job.stage != shared::GenerationStage::Materialize
                && result.job.stage != shared::GenerationStage::HeightTile) {
                throw std::logic_error{"typed terrain worker received an unsupported generation stage"};
            }
            result.tile = sampleHeightTile(
                *terrain_world,
                result.job.inherited_region,
                surface_tile_cache,
                world_epoch,
                {
                    .x = result.job.coordinate.x,
                    .y = result.job.coordinate.y,
                },
                cancellation
            );
            if (result.job.stage == shared::GenerationStage::Materialize) {
                result.chunk = terrain_world->materializeChunk(
                    result.tile.surface,
                    result.job.coordinate,
                    cancellation
                );
            }
            return;
        }
        if (generator == nullptr) {
            throw std::logic_error{ "legacy terrain worker has no generator" };
        }
        if (result.job.stage == shared::GenerationStage::Materialize) {
            result.chunk = generator->generateChunk(result.job.coordinate);
        } else if (result.job.stage == shared::GenerationStage::HeightTile) {
            result.tile = generator->generateHeightTile({
                .x = result.job.coordinate.x,
                .y = result.job.coordinate.y,
            });
        } else {
            result.generation_output = generateRegionOutput(result.job, *generator);
        }
    }

    static void executeJob(
        Result& result,
        shared::TerrainGenerator const* generator,
        shared::TerrainWorld const* terrain_world,
        SurfaceTileCache& surface_tile_cache,
        uint64_t const world_epoch,
        std::stop_token const& cancellation
    )
    {
        if (result.world_generation) {
            executeGenerationJob(
                result,
                generator,
                terrain_world,
                surface_tile_cache,
                world_epoch,
                cancellation
            );
            return;
        }

        if (terrain_world != nullptr) {
            if (!matchesTypedContext(result.job, *terrain_world, world_epoch, false)
                || !matchesLeaf(result.job.inherited_region, result.job, *terrain_world)) {
                throw std::logic_error{"typed preview work has no matching 64-block leaf lease"};
            }
            result.tile = sampleHeightTile(
                *terrain_world,
                result.job.inherited_region,
                surface_tile_cache,
                world_epoch,
                {
                    .x = result.job.coordinate.x,
                    .y = result.job.coordinate.y,
                },
                cancellation
            );
            return;
        }
        if (generator == nullptr) {
            throw std::logic_error{ "legacy preview worker has no generator" };
        }
        shared::HeightTileCoordinate const coordinate{
            .x = result.job.coordinate.x,
            .y = result.job.coordinate.y,
        };
        result.tile = generator->generateHeightTile(coordinate);
    }

    void submitPending()
    {
        while (!m_pending.empty() && m_submitted.size() < m_submission_window) {
            QueuedWork queued = std::move(m_pending.front());
            m_pending.erase(m_pending.begin());
            Work work = std::move(queued.work);
            std::shared_ptr<JobTiming> timing = std::move(queued.timing);
            std::function<void(core::ClientId, shared::HeightTileKey)> const completion_hook =
                m_before_height_tile_completion;
            std::shared_ptr<shared::TerrainWorld const> const terrain_world = m_terrain_world;
            std::shared_ptr<shared::TerrainGenerator const> const terrain_generator = m_terrain_generator;
            uint64_t const world_epoch = m_world_epoch;
            auto result = std::make_shared<Result>(Result{
                .client_id = work.client_id,
                .generation = work.generation,
                .job = work.job,
                .world_generation = work.world_generation,
                .generation_output = {},
            });
            if (timing != nullptr) {
                timing->submitted_at = std::chrono::steady_clock::now();
            }
            core::executor::Submission submission = m_executor->trySubmit(
                [this, result, completion_hook, terrain_world, terrain_generator, world_epoch, timing](
                    core::executor::CancellationToken const& token
                ) {
                    if (token.isCancellationRequested()) {
                        if (result->job.cancellation_source != nullptr) {
                            result->job.cancellation_source->request_stop();
                        }
                        result->cancelled = true;
                        return;
                    }
                    std::stop_token const cancellation = result->job.cancellation_source == nullptr
                        ? std::stop_token{}
                        : result->job.cancellation_source->get_token();
                    if (cancellation.stop_requested()) {
                        result->cancelled = true;
                        return;
                    }
                    if (timing != nullptr) {
                        recordStarted(*result, timing, std::chrono::steady_clock::now());
                    }
                    try {
                        executeJob(
                            *result,
                            terrain_generator.get(),
                            terrain_world.get(),
                            m_surface_tile_cache,
                            world_epoch,
                            cancellation
                        );
                        if (completion_hook && !result->world_generation && !result->cancelled) {
                            completion_hook(result->client_id, { result->job.coordinate.x, result->job.coordinate.y });
                        }
                    } catch (generation::GenerationCancelled const&) {
                        result->cancelled = true;
                    } catch (...) {
                        if (timing != nullptr) {
                            recordFinished(*result, timing, std::chrono::steady_clock::now());
                        }
                        throw;
                    }
                    if (timing != nullptr) {
                        recordFinished(*result, timing, std::chrono::steady_clock::now());
                    }
                }
            );
            if (submission.status != core::executor::SubmissionStatus::Accepted) {
                m_pending.insert(m_pending.begin(), QueuedWork{
                    .work = std::move(work),
                    .timing = std::move(timing),
                });
                break;
            }
            uint64_t const job_id = submission.handle.id();
            if (timing != nullptr) {
                AtomicJobMetrics& metrics = atomicMetrics(work.world_generation);
                metrics.submitted.fetch_add(1U, std::memory_order_relaxed);
                addDuration(
                    {
                        .total = metrics.enqueue_to_submit_total_ns,
                        .maximum = metrics.enqueue_to_submit_max_ns,
                    },
                    timing->submitted_at - timing->enqueued_at
                );
            }
            m_submitted.emplace(job_id, Submitted{
                .work = std::move(work),
                .result = std::move(result),
                .timing = std::move(timing),
                .handle = std::move(submission.handle),
            });
        }
    }

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<shared::TerrainWorld const> m_terrain_world;
    std::shared_ptr<shared::TerrainGenerator const> m_terrain_generator;
    uint64_t m_world_epoch = 0U;
    std::unique_ptr<core::executor::Executor> m_executor;
    SurfaceTileCache m_surface_tile_cache;
    std::vector<QueuedWork> m_pending;
    std::unordered_map<uint64_t, Submitted> m_submitted;
    AtomicJobMetrics m_height_tile_metrics;
    AtomicJobMetrics m_world_generation_metrics;
    uint32_t m_submission_window = 0U;
    bool m_benchmark_metrics_enabled = false;
    std::function<void(core::ClientId, shared::HeightTileKey)> m_before_height_tile_completion;
};


HeightTileWorkerPool::HeightTileWorkerPool(
    std::shared_ptr<shared::TerrainWorld const> terrain_world,
    uint32_t const worker_count,
    uint64_t const world_epoch
)
    : m_impl{std::make_unique<Impl>(std::move(terrain_world), nullptr, Impl::Configuration{
        .worker_count = worker_count,
        .world_epoch = world_epoch,
    })}
{ }

HeightTileWorkerPool::HeightTileWorkerPool(
    std::shared_ptr<shared::TerrainGenerator const> legacy_generator,
    uint32_t const worker_count
)
    : m_impl{std::make_unique<Impl>(nullptr, std::move(legacy_generator), Impl::Configuration{
        .worker_count = worker_count,
        .world_epoch = 0U,
    })}
{ }

HeightTileWorkerPool::~HeightTileWorkerPool() = default;

bool HeightTileWorkerPool::canAccept() const
{
    return m_impl->canAccept();
}

HeightTileWorkerPool::QueueMetrics HeightTileWorkerPool::queueMetrics() const
{
    return m_impl->queueMetrics();
}

void HeightTileWorkerPool::setBenchmarkMetricsEnabled(bool const enabled)
{
    m_impl->setBenchmarkMetricsEnabled(enabled);
}

void HeightTileWorkerPool::setBeforeHeightTileCompletionHook(
    std::function<void(core::ClientId, shared::HeightTileKey)> hook
)
{
    m_impl->setBeforeHeightTileCompletionHook(std::move(hook));
}

bool HeightTileWorkerPool::enqueue(Work work)
{
    return m_impl->enqueue(std::move(work));
}

void HeightTileWorkerPool::cancel(core::ClientId const client_id, uint64_t const generation)
{
    m_impl->cancel(client_id, generation);
}

std::vector<shared::GenerationJob> HeightTileWorkerPool::cancelQueued(
    core::ClientId const client_id,
    uint64_t const generation
)
{
    return m_impl->cancelQueued(client_id, generation);
}

std::vector<shared::GenerationJob> HeightTileWorkerPool::cancelQueuedIf(
    core::ClientId const client_id,
    std::function<bool(Work const&)> const& should_cancel
)
{
    return m_impl->cancelQueuedIf(client_id, should_cancel);
}

void HeightTileWorkerPool::reorderQueued(
    core::ClientId const client_id,
    std::function<bool(Work const&, Work const&)> const& order
)
{
    m_impl->reorderQueued(client_id, order);
}

std::vector<HeightTileWorkerPool::Result> HeightTileWorkerPool::takeResults(
    uint32_t const maximum_results
)
{
    return m_impl->takeResults(maximum_results);
}


} // namespace server
