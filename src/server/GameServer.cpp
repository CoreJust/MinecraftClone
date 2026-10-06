#include <server/GameServer.hpp>

#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <core/executor/Executor.hpp>
#include <core/IO/Log.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>

namespace {

[[nodiscard]]
shared::PolicyDiagnostic policyDiagnostic(
    shared::PolicyDiagnosticCode const code,
    std::string source_id,
    std::string message
)
{
    return {
        .code = code,
        .source_id = std::move(source_id),
        .message = std::move(message),
    };
}

[[nodiscard]]
std::optional<shared::PolicyCapabilityKeyId> capabilityKey(
    shared::PolicyCapabilityRegistry const& registry,
    std::string_view const key
) noexcept
{
    for (shared::PolicyCapabilityKeyId index = 0U; index < registry.definitions.size(); ++index) {
        if (registry.definitions[index].key == key) {
            return index;
        }
    }
    return std::nullopt;
}

[[nodiscard]]
int32_t floorDivideByHeightTileSide(int32_t const value) noexcept
{
    int32_t result = value / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    if (value < 0 && value % static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH) != 0) {
        --result;
    }
    return result;
}

[[nodiscard]]
shared::HeightTileKey heightTileKeyForPlayer(shared::Player const& player) noexcept
{
    return shared::normalizeHeightTileKey({
        .x = floorDivideByHeightTileSide(player.x),
        .y = floorDivideByHeightTileSide(player.y),
    });
}

[[nodiscard]]
std::tuple<double, int32_t, int32_t> heightTilePriority(
    shared::HeightTileKey const center,
    int32_t const movement_x,
    int32_t const movement_y,
    shared::HeightTileKey const key
) noexcept
{
    return {
        shared::heightTileInterestPriority(
            center,
            static_cast<int8_t>(movement_x),
            static_cast<int8_t>(movement_y),
            key
        ),
        key.y,
        key.x,
    };
}

[[nodiscard]]
int32_t shortestWrappedChunkDelta(int32_t const target, int32_t const center) noexcept
{
    static constexpr int32_t CHUNK_COUNT = static_cast<int32_t>(
        shared::WorldExtent::WIDTH / shared::Chunk::SIDE_LENGTH
    );
    static constexpr int32_t HALF_WORLD = CHUNK_COUNT / 2;
    int32_t delta = target - center;
    if (delta > HALF_WORLD) {
        delta -= CHUNK_COUNT;
    } else if (delta < -HALF_WORLD) {
        delta += CHUNK_COUNT;
    }
    return delta;
}

[[nodiscard]]
uint32_t directionalPriorityScore(int64_t const projection) noexcept
{
    static constexpr int64_t PRIORITY_BIAS = 1'000'000;
    return static_cast<uint32_t>(std::clamp(
        PRIORITY_BIAS - projection,
        int64_t{0},
        PRIORITY_BIAS * 2
    ));
}

[[nodiscard]]
shared::GenerationPriorityScores generationPriorityScores(
    shared::Player const& player,
    shared::Direction const direction,
    shared::ChunkCoordinate const coordinate
) noexcept
{
    int32_t const center_x = player.x / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH);
    int32_t const center_y = player.y / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH);
    int32_t const center_z = player.z / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH);
    int32_t const delta_x = shortestWrappedChunkDelta(coordinate.x, center_x);
    int32_t const delta_y = shortestWrappedChunkDelta(coordinate.y, center_y);
    int32_t const delta_z = coordinate.z - center_z;
    int64_t const view_projection = static_cast<int64_t>(delta_x) * direction.view_x
        + static_cast<int64_t>(delta_y) * direction.view_y;
    int64_t const movement_projection = static_cast<int64_t>(delta_x) * static_cast<int8_t>(direction.x)
        + static_cast<int64_t>(delta_y) * static_cast<int8_t>(direction.y)
        + static_cast<int64_t>(delta_z) * static_cast<int8_t>(direction.z);
    return {
        .distance = static_cast<uint32_t>(std::abs(delta_x) + std::abs(delta_y) + std::abs(delta_z)),
        .view = directionalPriorityScore(view_projection),
        .movement = directionalPriorityScore(movement_projection),
    };
}

[[nodiscard]]
uint64_t nextHeightTileToken(uint64_t& next_token) noexcept
{
    uint64_t const token = next_token;
    ++next_token;
    if (next_token == 0U) {
        next_token = 1U;
    }
    return token;
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
                * job.region->extent / (2U * SAMPLE_SIDE);
            uint64_t const sample_y = static_cast<uint64_t>(2U * y + 1U)
                * job.region->extent / (2U * SAMPLE_SIDE);
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

struct GameServer::HeightTileWorkerPool final {
    static constexpr uint32_t MAX_OUTSTANDING_WORK = 128U;

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

    struct Work final {
        core::ClientId client_id;
        uint64_t generation;
        shared::GenerationJob job;
        bool world_generation = false;
        std::shared_ptr<JobTiming> timing;
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
        std::vector<uint8_t> generation_output;
        std::shared_ptr<JobTiming> timing;
    };

    struct Submitted final {
        Work work;
        std::shared_ptr<Result> result;
        core::executor::JobHandle handle;
    };

    struct QueueMetrics final {
        GameServer::WorkerMetrics height_tiles;
        GameServer::WorkerMetrics world_generation;
    };

    HeightTileWorkerPool()
    {
        uint32_t const worker_count = GameServer::terrainWorkerCount(std::thread::hardware_concurrency());
        // The admission bound keeps submitted work bounded while sustaining workers between main-loop refills.
        m_submission_window = MAX_OUTSTANDING_WORK;
        m_executor = std::make_unique<core::executor::Executor>(
            core::executor::ExecutorLimits{
                .queue_capacity = worker_count,
                .result_capacity = MAX_OUTSTANDING_WORK,
            },
            worker_count
        );
    }

    ~HeightTileWorkerPool()
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
        for (Work const& work : m_pending) {
            ++metricsFor(metrics, work.world_generation).pending_jobs;
        }
        for (auto const& [id, submitted] : m_submitted) {
            static_cast<void>(id);
            GameServer::WorkerMetrics& job_metrics = metricsFor(metrics, submitted.work.world_generation);
            ++job_metrics.submitted_total;
            if (submitted.result->timing == nullptr) {
                ++job_metrics.executor_queued_jobs;
                continue;
            }
            switch (submitted.result->timing->state.load(std::memory_order_acquire)) {
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
        return metrics;
    }

    void setBenchmarkMetricsEnabled(bool const enabled)
    {
        std::lock_guard lock{m_mutex};
        m_benchmark_metrics_enabled = enabled;
    }

    [[nodiscard]]
    bool enqueue(Work work)
    {
        std::lock_guard lock{m_mutex};
        if (outstandingCount() >= MAX_OUTSTANDING_WORK) {
            return false;
        }
        if (m_benchmark_metrics_enabled) {
            work.timing = std::make_shared<JobTiming>();
            work.timing->enqueued_at = std::chrono::steady_clock::now();
            atomicMetrics(work.world_generation).enqueued.fetch_add(1U, std::memory_order_relaxed);
        }
        m_pending.push_back(std::move(work));
        submitPending();
        return true;
    }

    void cancel(core::ClientId const client_id, uint64_t const generation)
    {
        std::lock_guard lock{m_mutex};
        std::erase_if(m_pending, [client_id, generation](Work const& work) {
            return !work.world_generation && work.client_id == client_id && work.generation == generation;
        });
        for (auto& [id, submitted] : m_submitted) {
            static_cast<void>(id);
            if (!submitted.work.world_generation && submitted.work.client_id == client_id
                && submitted.work.generation == generation) {
                static_cast<void>(submitted.handle.requestCancellation());
            }
        }
    }

    [[nodiscard]]
    std::vector<shared::GenerationJob> cancelQueued(
        core::ClientId const client_id,
        uint64_t const generation
    ) {
        std::vector<shared::GenerationJob> cancelled;
        std::lock_guard lock{m_mutex};
        std::erase_if(m_pending, [&](Work const& work) {
            if (work.world_generation || work.client_id != client_id || work.generation != generation) {
                return false;
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
    ) {
        std::vector<shared::GenerationJob> cancelled;
        std::lock_guard lock{m_mutex};
        std::erase_if(m_pending, [&](Work const& work) {
            if (work.world_generation || work.client_id != client_id || !should_cancel(work)) {
                return false;
            }
            cancelled.push_back(work.job);
            return true;
        });
        for (auto& [id, submitted] : m_submitted) {
            static_cast<void>(id);
            if (!submitted.work.world_generation && submitted.work.client_id == client_id
                && should_cancel(submitted.work)) {
                static_cast<void>(submitted.handle.requestCancellation());
            }
        }
        return cancelled;
    }

    void reorderQueued(
        core::ClientId const client_id,
        std::function<bool(Work const&, Work const&)> const& order
    ) {
        std::lock_guard lock{m_mutex};
        std::vector<Work> client_pending;
        client_pending.reserve(m_pending.size());
        for (Work const& work : m_pending) {
            if (!work.world_generation && work.client_id == client_id) {
                client_pending.push_back(work);
            }
        }
        std::ranges::stable_sort(client_pending, order);
        auto reordered = client_pending.begin();
        for (Work& work : m_pending) {
            if (!work.world_generation && work.client_id == client_id) {
                work = std::move(*reordered);
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
            std::shared_ptr<JobTiming> const timing = submitted->second.result->timing;
            if (timing != nullptr) {
                AtomicJobMetrics& job_metrics = atomicMetrics(submitted->second.work.world_generation);
                WorkState const state = timing->state.load(std::memory_order_acquire);
                if (state == WorkState::Finished) {
                    auto const collection_delay = std::chrono::steady_clock::now() - timing->finished_at;
                    addDuration(
                        job_metrics.completion_to_collection_total_ns,
                        job_metrics.completion_to_collection_max_ns,
                        collection_delay
                    );
                }
                job_metrics.collected.fetch_add(1U, std::memory_order_relaxed);
            }
            submitted->second.result->cancelled = completion->status()
                == core::executor::CompletionStatus::Cancelled;
            submitted->second.result->succeeded = completion->status()
                == core::executor::CompletionStatus::Succeeded;
            results.push_back(std::move(*submitted->second.result));
            m_submitted.erase(submitted);
        }
        submitPending();
        return results;
    }

private:
    [[nodiscard]]
    static GameServer::WorkerMetrics& metricsFor(QueueMetrics& metrics, bool const world_generation) noexcept
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
        std::atomic<uint64_t>& total,
        std::atomic<uint64_t>& maximum,
        std::chrono::steady_clock::duration const duration
    ) noexcept
    {
        auto const nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
        uint64_t const count = static_cast<uint64_t>(std::max<int64_t>(nanoseconds.count(), 0));
        total.fetch_add(count, std::memory_order_relaxed);
        updateMaximum(maximum, count);
    }

    static void copyAtomicMetrics(AtomicJobMetrics const& source, GameServer::WorkerMetrics& destination)
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

    void recordStarted(Result const& result, std::chrono::steady_clock::time_point const started_at)
    {
        if (result.timing == nullptr) {
            return;
        }
        result.timing->started_at = started_at;
        result.timing->state.store(WorkState::Running, std::memory_order_release);
        AtomicJobMetrics& metrics = atomicMetrics(result.world_generation);
        metrics.started.fetch_add(1U, std::memory_order_relaxed);
        addDuration(
            metrics.enqueue_to_start_total_ns,
            metrics.enqueue_to_start_max_ns,
            started_at - result.timing->enqueued_at
        );
        addDuration(
            metrics.executor_queue_total_ns,
            metrics.executor_queue_max_ns,
            started_at - result.timing->submitted_at
        );
    }

    void recordFinished(Result const& result, std::chrono::steady_clock::time_point const finished_at)
    {
        if (result.timing == nullptr) {
            return;
        }
        result.timing->finished_at = finished_at;
        result.timing->state.store(WorkState::Finished, std::memory_order_release);
        AtomicJobMetrics& metrics = atomicMetrics(result.world_generation);
        metrics.finished.fetch_add(1U, std::memory_order_relaxed);
        addDuration(
            metrics.execution_total_ns,
            metrics.execution_max_ns,
            finished_at - result.timing->started_at
        );
    }

    [[nodiscard]] uint32_t outstandingCount() const noexcept
    {
        return static_cast<uint32_t>(m_pending.size() + m_submitted.size());
    }

    static void executeGenerationJob(Result& result, shared::TerrainGenerator const& generator)
    {
        if (result.job.stage == shared::GenerationStage::Materialize) {
            result.chunk = generator.generateChunk(result.job.coordinate);
        } else if (result.job.stage == shared::GenerationStage::HeightTile) {
            result.tile = generator.generateHeightTile({
                .x = result.job.coordinate.x,
                .y = result.job.coordinate.y,
            });
        } else {
            result.generation_output = generateRegionOutput(result.job, generator);
        }
    }

    static void executeJob(Result& result, shared::TerrainGenerator const& generator)
    {
        if (result.world_generation) {
            executeGenerationJob(result, generator);
            return;
        }

        shared::HeightTileCoordinate const coordinate{
            .x = result.job.coordinate.x,
            .y = result.job.coordinate.y,
        };
        result.tile = generator.generateHeightTile(coordinate);
    }

    void submitPending()
    {
        while (!m_pending.empty() && m_submitted.size() < m_submission_window) {
            Work work = std::move(m_pending.front());
            m_pending.erase(m_pending.begin());
            auto result = std::make_shared<Result>(Result{
                .client_id = work.client_id,
                .generation = work.generation,
                .job = work.job,
                .world_generation = work.world_generation,
                .timing = work.timing,
            });
            if (result->timing != nullptr) {
                result->timing->submitted_at = std::chrono::steady_clock::now();
            }
            core::executor::Submission submission = m_executor->trySubmit(
                [this, result](core::executor::CancellationToken const token) {
                    if (token.isCancellationRequested()) {
                        return;
                    }
                    if (result->timing != nullptr) {
                        recordStarted(*result, std::chrono::steady_clock::now());
                    }
                    thread_local shared::TerrainGenerator terrain_generator;
                    try {
                        executeJob(*result, terrain_generator);
                    } catch (...) {
                        if (result->timing != nullptr) {
                            recordFinished(*result, std::chrono::steady_clock::now());
                        }
                        throw;
                    }
                    if (result->timing != nullptr) {
                        recordFinished(*result, std::chrono::steady_clock::now());
                    }
                }
            );
            if (submission.status != core::executor::SubmissionStatus::Accepted) {
                m_pending.insert(m_pending.begin(), std::move(work));
                break;
            }
            uint64_t const job_id = submission.handle.id();
            if (work.timing != nullptr) {
                AtomicJobMetrics& metrics = atomicMetrics(work.world_generation);
                metrics.submitted.fetch_add(1U, std::memory_order_relaxed);
                addDuration(
                    metrics.enqueue_to_submit_total_ns,
                    metrics.enqueue_to_submit_max_ns,
                    work.timing->submitted_at - work.timing->enqueued_at
                );
            }
            m_submitted.emplace(job_id, Submitted{
                .work = std::move(work),
                .result = std::move(result),
                .handle = std::move(submission.handle),
            });
        }
    }

private:
    mutable std::mutex m_mutex;
    std::unique_ptr<core::executor::Executor> m_executor;
    std::vector<Work> m_pending;
    std::unordered_map<uint64_t, Submitted> m_submitted;
    AtomicJobMetrics m_height_tile_metrics;
    AtomicJobMetrics m_world_generation_metrics;
    uint32_t m_submission_window = 0U;
    bool m_benchmark_metrics_enabled = false;
};

GameServer::GameServer(
    uint16_t const port,
    std::vector<SpawnPoint> spawn_points,
    shared::WorldMode const world_mode,
    shared::WorldConfiguration const configuration,
    uint32_t const render_distance
)
    : core::Server{core::Address::localhost(port), 4, 2}
    , m_world{world_mode, configuration}
    , m_physics_world{shared::SparseWorldOptions{
        .seed = configuration.seed,
        .revision = configuration.algorithm_version,
    }}
    , m_world_generation{32U, configuration.algorithm_version, configuration.seed}
    , m_terrain_generator{std::make_shared<shared::TerrainGenerator const>()}
    , m_generation_plan{m_terrain_generator->generationPlan()}
    , m_spawn_points{checkedSpawnPoints(std::move(spawn_points), world_mode)}
    , m_height_tile_interest_orders{shared::prepareHeightTileInterestOrders(render_distance)}
{
    m_world.setCollisionWorld(&m_physics_world);
    m_height_tile_workers = std::make_unique<HeightTileWorkerPool>();
}

GameServer::~GameServer() = default;

std::expected<uint64_t, shared::PolicyDiagnostic> GameServer::publishPermissions(
    shared::PolicyCompilation compilation,
    shared::PolicyCapabilityRegistry registry
)
{
    std::optional<shared::PolicyCapabilityKeyId> const flight_permission = capabilityKey(
        registry,
        "minecraft:flight"
    );
    std::optional<shared::PolicyCapabilityKeyId> const collision_bypass_permission = capabilityKey(
        registry,
        "minecraft:collision-bypass"
    );
    if (!flight_permission.has_value() || !collision_bypass_permission.has_value()) {
        return std::unexpected(policyDiagnostic(
            shared::PolicyDiagnosticCode::InvalidDeclaration,
            std::string{compilation.plan.sourceId()},
            "movement permission registry must define flight and collision bypass"
        ));
    }
    for (shared::PolicyCapabilityKeyId const key : {*flight_permission, *collision_bypass_permission}) {
        shared::PolicyCapabilityDefinition const& definition = registry.definitions[key];
        if (definition.minimum_value != 0 || definition.maximum_value != 1
            || definition.hard_restriction != shared::PolicyRestriction::Maximum) {
            return std::unexpected(policyDiagnostic(
                shared::PolicyDiagnosticCode::InvalidDeclaration,
                std::string{compilation.plan.sourceId()},
                "movement permissions must be boolean capabilities with maximum hard restrictions"
            ));
        }
    }

    std::vector<shared::PolicySubject> const subjects = permissionSubjects();
    auto capabilities = m_permission_host.materialize(compilation.plan, subjects, registry);
    if (!capabilities) {
        return std::unexpected(capabilities.error());
    }
    if (!canApplyPublishedPermissions(*capabilities, registry)) {
        return std::unexpected(policyDiagnostic(
            shared::PolicyDiagnosticCode::InvalidDeclaration,
            std::string{compilation.plan.sourceId()},
            "movement policy is invalid for an active player or would strand a player in solid geometry"
        ));
    }

    auto const published = m_permission_host.publish(std::move(compilation), std::move(*capabilities));
    if (!published) {
        return std::unexpected(published.error());
    }
    m_permission_registry = std::move(registry);
    m_flight_permission = flight_permission;
    m_collision_bypass_permission = collision_bypass_permission;
    m_permissions_published = true;
    applyPublishedPermissions();
    return *published;
}

void GameServer::run()
{
    std::atomic_bool const never_stop{ false };
    run(never_stop);
}

void GameServer::run(std::atomic_bool const& stop_requested)
{
    run(stop_requested, nullptr);
}

void GameServer::run(
    std::atomic_bool const& stop_requested,
    BenchmarkHooks const* const benchmark_hooks
)
{
    m_benchmark_metrics_enabled = benchmark_hooks != nullptr && static_cast<bool>(benchmark_hooks->on_preview_metrics);
    m_height_tile_workers->setBenchmarkMetricsEnabled(m_benchmark_metrics_enabled);
    auto next_simulation = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point previous_loop_started_at{};
    std::chrono::nanoseconds previous_sleep{};
    while (!stop_requested.load(std::memory_order_relaxed)) {
        auto const now = std::chrono::steady_clock::now();
        auto const loop_started_at = m_benchmark_metrics_enabled
            ? now : std::chrono::steady_clock::time_point{};
        auto const loop_interval = m_benchmark_metrics_enabled && previous_loop_started_at.time_since_epoch().count() != 0
            ? std::chrono::duration_cast<std::chrono::nanoseconds>(loop_started_at - previous_loop_started_at)
            : std::chrono::nanoseconds::zero();
        if (m_benchmark_metrics_enabled) {
            previous_loop_started_at = loop_started_at;
        }
        std::chrono::nanoseconds tick_duration{};
        if (now >= next_simulation) {
            bool const measure_tick = benchmark_hooks != nullptr
                && (m_benchmark_metrics_enabled || static_cast<bool>(benchmark_hooks->on_tick));
            auto const tick_started_at = measure_tick
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            uint64_t const events = tick(std::chrono::milliseconds::zero());
            if (measure_tick) {
                tick_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - tick_started_at
                );
                if (benchmark_hooks != nullptr && benchmark_hooks->on_tick) {
                    benchmark_hooks->on_tick(tick_duration, events);
                }
            }
            next_simulation += shared::TICK;
        } else {
            while (poll(std::chrono::milliseconds::zero()) > 0) {
            }
            processHeightTileStreams();
        }
        if (benchmark_hooks && benchmark_hooks->on_preview_buffered) {
            for (PreviewStream const& stream : m_preview_streams) {
                benchmark_hooks->on_preview_buffered(
                    stream.client_id,
                    static_cast<uint32_t>(stream.ready_tiles.size() + stream.dispatched_keys.size())
                );
            }
        }
        if (benchmark_hooks && benchmark_hooks->on_preview_metrics) {
            HeightTileWorkerPool::QueueMetrics const worker_queue_metrics = m_height_tile_workers->queueMetrics();
            auto const metrics_at = std::chrono::steady_clock::now();
            std::chrono::nanoseconds const loop_work = std::chrono::duration_cast<std::chrono::nanoseconds>(
                metrics_at - loop_started_at
            );
            for (PreviewStream const& stream : m_preview_streams) {
                PreviewStreamMetrics metrics{
                    .height_tile_jobs = worker_queue_metrics.height_tiles,
                    .world_generation_jobs = worker_queue_metrics.world_generation,
                    .queued_tiles = static_cast<uint32_t>(stream.queued_keys.size()),
                    .dispatched_tiles = static_cast<uint32_t>(stream.dispatched_keys.size()),
                    .pending_worker_jobs = worker_queue_metrics.height_tiles.pending_jobs
                        + worker_queue_metrics.world_generation.pending_jobs,
                    .submitted_worker_jobs = worker_queue_metrics.height_tiles.executor_queued_jobs
                        + worker_queue_metrics.height_tiles.running_jobs
                        + worker_queue_metrics.height_tiles.completed_uncollected_jobs
                        + worker_queue_metrics.world_generation.executor_queued_jobs
                        + worker_queue_metrics.world_generation.running_jobs
                        + worker_queue_metrics.world_generation.completed_uncollected_jobs,
                    .ready_tiles = static_cast<uint32_t>(stream.ready_tiles.size()),
                    .inflight_deliveries = static_cast<uint32_t>(stream.inflight_deliveries.size()),
                    .inflight_additions = static_cast<uint32_t>(stream.inflight_addition_keys.size()),
                    .delivery_credits = stream.delivery_credits,
                    .resident_tiles = static_cast<uint32_t>(stream.resident_keys.size()),
                    .delivery_credit_samples = stream.delivery_credit_samples,
                    .delivery_credit_total = stream.delivery_credit_total,
                    .delivery_credit_max = stream.delivery_credit_max,
                    .server_loop_interval = loop_interval,
                    .server_loop_work = loop_work,
                    .server_tick = tick_duration,
                    .stream_pump = m_last_stream_pump_duration,
                    .previous_sleep = previous_sleep,
                };
                auto const replication = std::ranges::find(
                    m_player_replications,
                    stream.client_id,
                    &PlayerReplication::id
                );
                if (replication != m_player_replications.end()) {
                    metrics.latest_received_input_sequence = replication->latest_received_sequence;
                    metrics.acknowledged_input_sequence = replication->acknowledged_input_sequence;
                    metrics.unacknowledged_input_count = replication->latest_received_sequence
                        - replication->acknowledged_input_sequence;
                    metrics.pending_input_count = static_cast<uint32_t>(replication->pending_inputs.size());
                    metrics.materialization_admission_failures = replication->materialization_admission_failures;
                }
                benchmark_hooks->on_preview_metrics(stream.client_id, metrics);
            }
        }
        auto const sleep_deadline = std::min(
            next_simulation,
            std::chrono::steady_clock::now() + std::chrono::milliseconds{5}
        );
        auto const sleep_started_at = m_benchmark_metrics_enabled
            ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        std::this_thread::sleep_until(sleep_deadline);
        if (m_benchmark_metrics_enabled) {
            previous_sleep = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - sleep_started_at
            );
        }
    }
}

std::chrono::milliseconds GameServer::fixedTickDelay(std::chrono::milliseconds const elapsed) noexcept
{
    return elapsed < shared::TICK ? shared::TICK - elapsed : std::chrono::milliseconds::zero();
}

uint32_t GameServer::terrainWorkerCount(uint32_t const hardware_concurrency) noexcept
{
    static constexpr uint32_t RESERVED_SERVER_THREADS{ 1U };
    static constexpr uint32_t MAXIMUM_TERRAIN_WORKERS{ 8U };
    uint32_t const available = hardware_concurrency > RESERVED_SERVER_THREADS
        ? hardware_concurrency - RESERVED_SERVER_THREADS
        : 1U;
    return std::min(available, MAXIMUM_TERRAIN_WORKERS);
}

uint64_t GameServer::tick(std::chrono::milliseconds const timeout) {
    for (PlayerReplication& replication : m_player_replications) {
        replication.action_consumed_this_tick = false;
    }
    uint64_t events = static_cast<uint64_t>(poll(timeout));
    while (true) {
        uint32_t const drained = poll(std::chrono::milliseconds::zero());
        if (drained == 0U) {
            break;
        }
        events += static_cast<uint64_t>(drained);
    }
    for (PlayerReplication& replication : m_player_replications) {
        if (!replication.action_consumed_this_tick && !replication.pending_inputs.empty()) {
            shared::ClientInputMessage const input = replication.pending_inputs.front();
            replication.pending_inputs.pop_front();
            if (!processInput(replication, input)) {
                replication.pending_inputs.push_front(input);
            }
        }
    }
    if (m_world.mode() == shared::WorldMode::Flight) {
        for (PlayerReplication const& replication : m_player_replications) {
            auto const player = m_world.player(replication.id);
            if (!player.has_value()) {
                continue;
            }
            shared::ChunkCoordinate const coordinate{
                .x = player->x / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH),
                .y = player->y / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH),
                .z = player->z / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH),
            };
            if (shared::WorldBounds::isValidChunk(coordinate)
                && m_physics_world.residentChunk(coordinate) == nullptr) {
                static_cast<void>(m_world_generation.requestPlan(coordinate, m_generation_plan));
            }
        }
    }
    processHeightTileStreams();
    return events;
}

std::expected<std::vector<GameServer::SpawnPoint>, std::string> GameServer::validateSpawnPoints(
    std::vector<SpawnPoint> spawn_points,
    shared::WorldMode const world_mode
) {
    for (SpawnPoint const& spawn_point : spawn_points) {
        bool const valid_character = spawn_point.character == '@'
            || spawn_point.character == '#'
            || spawn_point.character == '$'
            || spawn_point.character == '%'
            || spawn_point.character == '&';
        if (!valid_character) {
            return std::unexpected("spawn point has an unsupported character");
        }
        bool const outside_flat_world = world_mode == shared::WorldMode::Flat
            && (spawn_point.x < 0 || spawn_point.x > shared::World::MAX_PLAYER_ORIGIN_CELL
                || spawn_point.y < 0 || spawn_point.y > shared::World::MAX_PLAYER_ORIGIN_CELL
                || spawn_point.z != 0);
        bool const outside_flight_world = world_mode == shared::WorldMode::Flight
            && (spawn_point.z < shared::World::FLIGHT_MIN_CELL
                || spawn_point.z > shared::World::FLIGHT_MAX_Z);
        if (outside_flat_world || outside_flight_world) {
            return std::unexpected("spawn point is outside the world");
        }
    }
    for (auto first = spawn_points.begin(); first != spawn_points.end(); ++first) {
        for (auto second = std::next(first); second != spawn_points.end(); ++second) {
            if (first->character == second->character) {
                return std::unexpected("spawn points contain duplicate characters");
            }
            int32_t const horizontal_distance = std::abs(first->x - second->x);
            int32_t const vertical_distance = std::abs(first->y - second->y);
            if (world_mode == shared::WorldMode::Flat
                && horizontal_distance <= 1 && vertical_distance <= 1) {
                return std::unexpected("spawn points overlap player collision neighborhoods");
            }
        }
    }
    return spawn_points;
}

std::vector<GameServer::SpawnPoint> GameServer::checkedSpawnPoints(
    std::vector<SpawnPoint> spawn_points,
    shared::WorldMode const world_mode
) {
    auto validated = validateSpawnPoints(
        std::move(spawn_points),
        world_mode
    );
    if (!validated.has_value()) {
        throw std::invalid_argument{ validated.error() };
    }
    return std::move(*validated);
}

void GameServer::onConnected(core::ServerConnectEvent const client) {
    CORE_INFO("Server: onConnected {}", client.client.address());
}

void GameServer::onDisconnected(core::ServerDisconnectEvent const client) {
    CORE_INFO("Server: onDisconnected {}", client.client.address());
    std::erase_if(m_preview_streams, [this, &client](PreviewStream& stream) {
        if (stream.client_id != client.client_id) {
            return false;
        }
        m_height_tile_workers->cancel(stream.client_id, stream.generation);
        stream.scheduler.invalidateRevision(stream.generation);
        return true;
    });
    auto const player = m_world.player(client.client_id);
    if (!player.has_value()) {
        return;
    }
    send(shared::ServerRemovePlayerMessage{
        .ch = player->ch,
    });
    m_world.despawnPlayer(client.client_id);
    std::erase_if(m_player_replications, [&client](PlayerReplication const& replication) {
        return replication.id == client.client_id;
    });
    if (m_permissions_published) {
        auto const refreshed = refreshPublishedPermissions(
            static_cast<shared::PolicyEntityId>(client.client_id)
        );
        if (!refreshed) {
            CORE_ERROR("Failed to refresh permissions after player departure: {}", refreshed.error().message);
        }
    }
}

void GameServer::onReceived(core::ServerReceiveEvent event) {
    std::optional maybe_msg = shared::decodeMessage(event.data);
    if (!maybe_msg) {
        CORE_ERROR("Received a corrupted message");
        return;
    }

    shared::PlayerId const id = event.client_id;
    shared::Message* msg_ptr = &*maybe_msg;
    if (auto* msg = std::get_if<shared::JoinRequestMessage>(msg_ptr)) {
        if (msg->mode != m_world.mode() || msg->configuration != m_world.configuration()) {
            sendTo(id, shared::JoinResponseMessage{
                .accepted = false,
            });
            return;
        }
        char const ch = msg->ch;
        if (m_world.player(id) || m_world.playerExists(ch)) {
            sendTo(id, shared::JoinResponseMessage{
                .accepted = false,
            });
            return;
        }
        auto const spawn_point = std::ranges::find(
            m_spawn_points,
            ch,
            &SpawnPoint::character
        );
        if (spawn_point == m_spawn_points.end()) {
            m_world.spawnPlayer(id, ch, std::nullopt, shared::defaultPlayerPaletteIndex(ch));
        } else {
            m_world.spawnPlayer(id, ch, shared::PlayerPosition{
                .x = spawn_point->x,
                .y = spawn_point->y,
                .z = spawn_point->z,
            }, shared::defaultPlayerPaletteIndex(ch));
        }
        m_player_replications.push_back(PlayerReplication{ .id = id });
        if (m_permissions_published) {
            auto const refreshed = refreshPublishedPermissions();
            if (!refreshed) {
                CORE_ERROR("Rejected player join because active permissions could not be materialized: {}",
                    refreshed.error().message);
                m_world.despawnPlayer(id);
                std::erase_if(m_player_replications, [id](PlayerReplication const& replication) {
                    return replication.id == id;
                });
                sendTo(id, shared::JoinResponseMessage{
                    .accepted = false,
                });
                return;
            }
        }
        sendTo(id, shared::JoinResponseMessage{
            .accepted = true,
        });

        shared::Player const p = m_world.player(id).value();
        CORE_INFO("Player '{}' spawned at x {}, y {}, z {}", ch, p.x, p.y, p.z);
        for (shared::Player const& player : m_world.players()) {
            PlayerReplication* const replication = playerReplication(player.id);
            if (replication == nullptr) {
                continue;
            }
            shared::ServerPlayerPositionMessage const position = playerPositionMessage(player, *replication);
            if (player.id == id) {
                send(position);
            } else {
                sendTo(id, position);
            }
        }
        if (msg->wants_previews) {
            startHeightTileStream(id);
        }
    } else if (auto* msg = std::get_if<shared::ClientInputMessage>(msg_ptr)) {
        auto const player = m_world.player(id);
        PlayerReplication* const replication = playerReplication(id);
        if (!player || replication == nullptr) {
            return;
        }
        if (replication->has_received_sequence
            && !shared::isNewerSequence(msg->sequence, replication->latest_received_sequence)) {
            return;
        }
        if ((replication->action_consumed_this_tick || !replication->pending_inputs.empty())
            && replication->pending_inputs.size() == PlayerReplication::MAX_PENDING_INPUTS) {
            return;
        }
        replication->latest_received_sequence = msg->sequence;
        replication->has_received_sequence = true;
        if (!replication->action_consumed_this_tick && replication->pending_inputs.empty()) {
            if (!processInput(*replication, *msg)) {
                replication->pending_inputs.push_back(*msg);
            }
        } else {
            replication->pending_inputs.push_back(*msg);
        }
    } else if (auto* msg = std::get_if<shared::ClientHeightTileCreditMessage>(msg_ptr)) {
        acknowledgeHeightTileDelivery(id, *msg);
    } else {
        CORE_ERROR("Received a message unsupported by the server {}", msg_ptr->index());
    }
}

bool GameServer::processInput(PlayerReplication& replication, shared::ClientInputMessage const input)
{
    shared::Player const player_before_input = *m_world.player(replication.id);
    bool capabilities_changed = false;
    if (input.direction.cycle_movement_capabilities) {
        uint8_t next_bits = player_before_input.movement_capabilities.bits == 3U
            ? 1U : (player_before_input.movement_capabilities.bits == 1U ? 0U : 3U);
        if (m_permissions_published) {
            std::shared_ptr<shared::PolicySnapshot const> const policy = m_permission_host.snapshot();
            std::shared_ptr<shared::PolicyCapabilitySnapshot const> const effective = policy
                ? policy->capabilities() : nullptr;
            if (effective == nullptr || !m_flight_permission.has_value()
                || !m_collision_bypass_permission.has_value()) {
                next_bits = 0U;
            } else {
                uint8_t allowed_bits = effective->allows(replication.id, *m_flight_permission) ? 1U : 0U;
                if ((allowed_bits & 1U) != 0U
                    && effective->allows(replication.id, *m_collision_bypass_permission)) {
                    allowed_bits |= 2U;
                }
                next_bits &= allowed_bits;
            }
        }
        capabilities_changed = m_world.setPlayerMovementCapabilities(
            replication.id,
            { .bits = next_bits }
        );
    }
    shared::Player const player_before_movement = *m_world.player(replication.id);
    bool const should_simulate = input.direction.x != 0 || input.direction.y != 0 || input.direction.z != 0
        || !player_before_movement.movement_capabilities.allows(shared::MovementCapability::Flight);
    bool moved = false;
    bool generation_failed = false;
    if (should_simulate) {
        uint64_t const revision = m_world.configuration().algorithm_version;
        uint64_t const seed = m_world.configuration().seed;
        std::vector<shared::ChunkCoordinate> missing_chunks;
        for (shared::ChunkCoordinate const coordinate : m_world.flightCollisionChunks(
                 replication.id,
                 input.direction
             )) {
            if (m_physics_world.queryBlock({
                    .x = static_cast<int64_t>(coordinate.x) * shared::Chunk::SIDE_LENGTH,
                    .y = static_cast<int64_t>(coordinate.y) * shared::Chunk::SIDE_LENGTH,
                    .z = static_cast<int64_t>(coordinate.z) * shared::Chunk::SIDE_LENGTH,
                }).state == shared::GenerationState::Materialized) {
                continue;
            }
            shared::GenerationState const state = m_world_generation.state(coordinate, revision, seed);
            if (state == shared::GenerationState::Failed
                && !m_world_generation.requestRetry(coordinate, revision, seed)) {
                generation_failed = true;
                break;
            }
            if (state == shared::GenerationState::Cancelled) {
                generation_failed = true;
                break;
            }
            missing_chunks.push_back(coordinate);
        }
        bool const plan_materializes_chunks = std::ranges::find(
            m_generation_plan.chunk_stages,
            shared::GenerationStage::Materialize
        ) != m_generation_plan.chunk_stages.end();
        if (!missing_chunks.empty() && !plan_materializes_chunks) {
            generation_failed = true;
        }
        bool queue_full = false;
        if (!generation_failed) {
            for (shared::ChunkCoordinate const coordinate : missing_chunks) {
                shared::GenerationAdmission const admission = m_world_generation.requestPlan(
                    coordinate,
                    m_generation_plan,
                    generationPriorityScores(player_before_movement, input.direction, coordinate)
                );
                if (admission == shared::GenerationAdmission::InvalidPlan) {
                    generation_failed = true;
                    break;
                }
                if (admission == shared::GenerationAdmission::QueueFull) {
                    queue_full = true;
                    if (++replication.materialization_admission_failures
                        >= PlayerReplication::MAX_MATERIALIZATION_ADMISSION_FAILURES) {
                        generation_failed = true;
                    }
                    break;
                }
            }
        }
        if (!queue_full) {
            replication.materialization_admission_failures = 0U;
        }
        if (!missing_chunks.empty() && !generation_failed) {
            if (capabilities_changed) {
                static_cast<void>(m_world.setPlayerMovementCapabilities(
                    replication.id,
                    player_before_input.movement_capabilities
                ));
            }
            return false;
        }
        if (!generation_failed) {
            moved = m_world.movePlayer(replication.id, input.direction);
        }
    }
    replication.materialization_admission_failures = 0U;
    replication.action_consumed_this_tick = true;
    replication.acknowledged_input_sequence = input.sequence;
    auto const stream = std::ranges::find(m_preview_streams, replication.id, &PreviewStream::client_id);
    if (stream != m_preview_streams.end()) {
        int8_t const movement_x = static_cast<int8_t>(input.direction.x);
        int8_t const movement_y = static_cast<int8_t>(input.direction.y);
        stream->heading_x = movement_x != 0 || movement_y != 0 ? movement_x : input.direction.view_x;
        stream->heading_y = movement_x != 0 || movement_y != 0 ? movement_y : input.direction.view_y;
    }
    ++replication.state_revision;
    shared::ServerPlayerPositionMessage const position = playerPositionMessage(
        *m_world.player(replication.id),
        replication
    );
    if (moved || capabilities_changed) {
        send(position);
    } else {
        sendTo(replication.id, position);
    }
    // Submit the latency-sensitive acknowledgement before this tick admits
    // additional reliable terrain fragments on the bulk channel.
    flush();
    return true;
}

GameServer::PlayerReplication* GameServer::playerReplication(shared::PlayerId const id) noexcept
{
    for (PlayerReplication& replication : m_player_replications) {
        if (replication.id == id) {
            return &replication;
        }
    }
    return nullptr;
}

shared::ServerPlayerPositionMessage GameServer::playerPositionMessage(
    shared::Player const& player,
    PlayerReplication const& replication
) const noexcept
{
    return {
        .ch = player.ch,
        .palette_index = player.palette_index,
        .movement_capabilities = player.movement_capabilities,
        .x = player.x,
        .y = player.y,
        .z = player.z,
        .x_subcell = player.x_subcell,
        .y_subcell = player.y_subcell,
        .z_subcell = player.z_subcell,
        .vertical_velocity_subcells = player.vertical_velocity_subcells,
        .acknowledged_input_sequence = replication.acknowledged_input_sequence,
        .state_revision = replication.state_revision,
    };
}

void GameServer::send(shared::Message const message) {
    sendTo(std::nullopt, std::move(message));
}

void GameServer::sendTo(std::optional<core::ClientId> const client_id, shared::Message message) {
    std::vector const message_bytes = shared::encodeMessage(std::move(message));
    std::optional<core::Peer> peer;
    if (client_id.has_value()) {
        peer = client(*client_id);
        if (!peer) {
            CORE_ERROR("Cannot send a message to disconnected client {}", *client_id);
            return;
        }
    }
    if (!core::Server::send(peer, message_bytes, shared::GAME_CHANNEL, core::SendMode{ core::SendMode::Reliable })) {
        CORE_ERROR("Failed to send a message");
    }
}

std::vector<shared::PolicySubject> GameServer::permissionSubjects() const
{
    std::vector<shared::PolicySubject> subjects;
    subjects.reserve(m_world.players().size());
    for (shared::Player const& player : m_world.players()) {
        subjects.push_back({
            .id = player.id,
            .entity_class = shared::PolicyEntityClass::Player,
            .kind = "player",
        });
    }
    return subjects;
}

bool GameServer::canApplyPublishedPermissions(
    shared::PolicyCapabilitySnapshot const& capabilities,
    shared::PolicyCapabilityRegistry const& registry
) const
{
    std::optional<shared::PolicyCapabilityKeyId> const flight_permission = capabilityKey(
        registry,
        "minecraft:flight"
    );
    std::optional<shared::PolicyCapabilityKeyId> const collision_bypass_permission = capabilityKey(
        registry,
        "minecraft:collision-bypass"
    );
    if (!flight_permission.has_value() || !collision_bypass_permission.has_value()) {
        return false;
    }
    for (shared::Player const& player : m_world.players()) {
        std::optional<int64_t> const flight = capabilities.value(player.id, *flight_permission);
        std::optional<int64_t> const collision_bypass = capabilities.value(player.id, *collision_bypass_permission);
        if (!flight.has_value() || !collision_bypass.has_value()
            || (*flight != 0 && *flight != 1)
            || (*collision_bypass != 0 && *collision_bypass != 1)) {
            return false;
        }
        shared::MovementCapabilities const movement{
            .bits = static_cast<uint8_t>((*flight != 0 ? 1U : 0U) | (*collision_bypass != 0 ? 2U : 0U)),
        };
        if (!shared::isValidMovementCapabilities(movement)
            || !m_world.canSetPlayerMovementCapabilities(player.id, movement)) {
            return false;
        }
    }
    return true;
}

std::expected<uint64_t, shared::PolicyDiagnostic> GameServer::refreshPublishedPermissions(
    std::optional<shared::PolicyEntityId> const expired_subject
)
{
    std::shared_ptr<shared::PolicySnapshot const> const active = m_permission_host.snapshot();
    if (!m_permissions_published || active == nullptr) {
        return 0U;
    }
    shared::PolicyCompilation compilation{
        .plan = active->plan(),
        .jit_mode = active->jitMode(),
        .jit_prepared = active->jitPrepared(),
        .interpreter_parity = active->interpreterParity(),
    };
    if (expired_subject.has_value()) {
        compilation.plan = compilation.plan.withoutSubject(*expired_subject);
    }
    std::vector<shared::PolicySubject> const subjects = permissionSubjects();
    auto capabilities = m_permission_host.materialize(compilation.plan, subjects, m_permission_registry);
    if (!capabilities) {
        return std::unexpected(capabilities.error());
    }
    if (!canApplyPublishedPermissions(*capabilities, m_permission_registry)) {
        return std::unexpected(policyDiagnostic(
            shared::PolicyDiagnosticCode::InvalidDeclaration,
            std::string{compilation.plan.sourceId()},
            "active movement policy is invalid for an active player or would strand a player in solid geometry"
        ));
    }
    auto const published = m_permission_host.publish(std::move(compilation), std::move(*capabilities));
    if (!published) {
        return std::unexpected(published.error());
    }
    applyPublishedPermissions();
    return *published;
}

void GameServer::applyPublishedPermissions()
{
    std::shared_ptr<shared::PolicySnapshot const> const active = m_permission_host.snapshot();
    if (active == nullptr || !m_flight_permission.has_value() || !m_collision_bypass_permission.has_value()) {
        return;
    }
    std::shared_ptr<shared::PolicyCapabilitySnapshot const> const capabilities = active->capabilities();
    if (capabilities == nullptr) {
        return;
    }
    for (shared::Player const& player : m_world.players()) {
        std::optional<int64_t> const flight = capabilities->value(player.id, *m_flight_permission);
        std::optional<int64_t> const collision_bypass = capabilities->value(
            player.id,
            *m_collision_bypass_permission
        );
        if (!flight.has_value() || !collision_bypass.has_value()) {
            continue;
        }
        shared::MovementCapabilities const movement{
            .bits = static_cast<uint8_t>((*flight != 0 ? 1U : 0U) | (*collision_bypass != 0 ? 2U : 0U)),
        };
        if (movement == player.movement_capabilities) {
            continue;
        }
        if (!m_world.setPlayerMovementCapabilities(player.id, movement)) {
            CORE_ERROR("Validated permission update could not be applied to player {}", player.id);
            continue;
        }
        PlayerReplication* const replication = playerReplication(player.id);
        if (replication != nullptr) {
            ++replication->state_revision;
            send(playerPositionMessage(*m_world.player(player.id), *replication));
        }
    }
}

void GameServer::sendHeightTileTo(
    std::optional<core::ClientId> const client_id,
    shared::Message message
) {
    std::vector const message_bytes = shared::encodeMessage(std::move(message));
    std::optional<core::Peer> peer;
    if (client_id.has_value()) {
        peer = client(*client_id);
        if (!peer) {
            return;
        }
    }
    if (!core::Server::send(peer, message_bytes, shared::HEIGHT_TILE_CHANNEL, core::SendMode{ core::SendMode::Reliable })) {
        CORE_ERROR("Failed to send a height tile message");
    }
}

void GameServer::startHeightTileStream(core::ClientId const client_id)
{
    sendHeightTileTo(client_id, shared::ServerHeightTileDescriptorMessage{
        .configuration = m_world.configuration(),
        .world_revision = PreviewStream::WORLD_REVISION,
        .max_height_tiles = shared::heightTileInterestCount(m_height_tile_interest_orders->radius),
        .max_height_tile_bytes = shared::HEIGHT_TILE_PAYLOAD_BYTES,
    });
    sendHeightTileTo(client_id, shared::ServerWorldRevisionMessage{ .world_revision = PreviewStream::WORLD_REVISION });
    m_preview_streams.emplace_back(client_id, m_height_tile_interest_orders->radius);
}

void GameServer::acknowledgeHeightTileDelivery(
    core::ClientId const client_id,
    shared::ClientHeightTileCreditMessage const& credit
)
{
    if (credit.world_revision != PreviewStream::WORLD_REVISION) {
        return;
    }
    auto const stream = std::ranges::find(m_preview_streams, client_id, &PreviewStream::client_id);
    if (stream == m_preview_streams.end()) {
        return;
    }
    if (credit.delivery_token == 0U) {
        if (credit.credits != shared::HEIGHT_TILE_DELIVERY_WINDOW
            || stream->delivery_credits != 0U || !stream->inflight_deliveries.empty()) {
            return;
        }
        stream->delivery_credits = credit.credits;
        return;
    }
    if (credit.credits != 1U) {
        return;
    }
    auto const delivery = stream->inflight_deliveries.find(credit.delivery_token);
    if (delivery == stream->inflight_deliveries.end()) {
        return;
    }
    if (delivery->second.admitted_at.time_since_epoch().count() != 0) {
        auto const elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - delivery->second.admitted_at
        );
        ++stream->delivery_credit_samples;
        stream->delivery_credit_total += elapsed;
        stream->delivery_credit_max = std::max(stream->delivery_credit_max, elapsed);
    }
    for (shared::HeightTileKey const key : delivery->second.additions) {
        stream->inflight_addition_keys.erase(key);
        stream->resident_keys.insert(key);
        if (!stream->desires(key) && !stream->inflight_removal_keys.contains(key)) {
            stream->pending_removals.insert(key);
        }
    }
    for (shared::HeightTileKey const key : delivery->second.removals) {
        stream->inflight_removal_keys.erase(key);
        stream->pending_removals.erase(key);
        stream->resident_keys.erase(key);
        if (stream->desires(key)) {
            stream->priority_cursor = 0U;
        }
    }
    stream->inflight_deliveries.erase(delivery);
    if (stream->delivery_credits < shared::HEIGHT_TILE_DELIVERY_WINDOW) {
        ++stream->delivery_credits;
    }
}

uint32_t GameServer::admitHeightTileDeliveries(
    PreviewStream& stream,
    uint32_t const maximum_batches
)
{
    // Credits are an upper bound, not a requirement to fill the transport window.
    // Four maximum-size batches retain 34,356 bytes, leaving gameplay headroom in
    // the default 65,536-byte ENet reliable window shared by both channels.
    uint32_t const available_batches = std::min<uint32_t>({
        stream.delivery_credits,
        static_cast<uint32_t>(PreviewStream::MAX_INFLIGHT_DELIVERIES - stream.inflight_deliveries.size()),
        maximum_batches,
    });
    uint32_t const maximum_operations = available_batches * shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY;
    if (maximum_operations == 0U) {
        return 0U;
    }

    std::vector<shared::HeightTileKey> const pending_removals{
        stream.pending_removals.begin(), stream.pending_removals.end()
    };
    std::vector<shared::HeightTileKey> const inflight_removals{
        stream.inflight_removal_keys.begin(), stream.inflight_removal_keys.end()
    };
    std::vector<shared::HeightTileKey> removal_keys = shared::selectHeightTileRemovalCandidates(
        stream.center,
        stream.heading_x,
        stream.heading_y,
        pending_removals,
        inflight_removals,
        available_batches
    );
    std::erase_if(removal_keys, [&stream](shared::HeightTileKey const key) {
        return stream.desires(key);
    });

    uint32_t const maximum_additions = maximum_operations - static_cast<uint32_t>(removal_keys.size());
    std::vector<shared::HeightTileKey> ready_keys;
    ready_keys.reserve(stream.ready_tiles.size());
    for (auto const& [key, tile] : stream.ready_tiles) {
        static_cast<void>(tile);
        if (stream.desires(key) && !stream.inflight_addition_keys.contains(key)) {
            ready_keys.push_back(key);
        }
    }
    auto const ready_order = [&stream](shared::HeightTileKey const first, shared::HeightTileKey const second) {
        return heightTilePriority(stream.center, stream.heading_x, stream.heading_y, first)
            < heightTilePriority(stream.center, stream.heading_x, stream.heading_y, second);
    };
    if (ready_keys.size() > maximum_additions) {
        std::ranges::partial_sort(
            ready_keys,
            ready_keys.begin() + maximum_additions,
            ready_order
        );
        ready_keys.resize(maximum_additions);
    } else {
        std::ranges::sort(ready_keys, ready_order);
    }

    auto ready = ready_keys.begin();
    auto removal = removal_keys.begin();
    uint32_t admitted_batches = 0U;
    while (stream.delivery_credits > 0U
        && stream.inflight_deliveries.size() < PreviewStream::MAX_INFLIGHT_DELIVERIES
        && admitted_batches < maximum_batches) {
        shared::ServerHeightTileBatchMessage batch{
            .delivery_token = nextHeightTileToken(m_next_height_tile_token),
        };
        PreviewStream::Delivery delivery;
        batch.tiles.reserve(shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY);
        batch.removals.reserve(shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY);
        while (ready != ready_keys.end()
            && batch.tiles.size() + batch.removals.size() < shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY) {
            shared::HeightTileKey const key = *ready;
            ++ready;
            auto const tile = stream.ready_tiles.find(key);
            if (tile == stream.ready_tiles.end()) {
                continue;
            }
            delivery.additions.push_back(key);
            stream.inflight_addition_keys.insert(key);
            batch.tiles.push_back(std::move(tile->second));
            stream.ready_tiles.erase(tile);
        }
        while (removal != removal_keys.end()
            && batch.tiles.size() + batch.removals.size() < shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY) {
            shared::HeightTileKey const key = *removal;
            ++removal;
            delivery.removals.push_back(key);
            stream.inflight_removal_keys.insert(key);
            batch.removals.push_back({
                .key = key,
                .revision = PreviewStream::WORLD_REVISION,
                .token = nextHeightTileToken(m_next_height_tile_token),
            });
        }
        if (delivery.additions.empty() && delivery.removals.empty()) {
            return admitted_batches;
        }
        uint64_t const delivery_token = batch.delivery_token;
        if (m_benchmark_metrics_enabled) {
            delivery.admitted_at = std::chrono::steady_clock::now();
        }
        stream.inflight_deliveries.emplace(delivery_token, std::move(delivery));
        --stream.delivery_credits;
        sendHeightTileTo(stream.client_id, std::move(batch));
        ++admitted_batches;
    }
    return admitted_batches;
}

void GameServer::refreshHeightTileInterest(PreviewStream& stream, shared::Player const& player)
{
    shared::HeightTileKey const next_center = heightTileKeyForPlayer(player);
    shared::HeightTileHeading const heading = shared::canonicalHeightTileHeading(
        stream.heading_x, stream.heading_y
    );
    if (stream.has_center && stream.center == next_center
        && stream.applied_heading_x == heading.x
        && stream.applied_heading_y == heading.y) {
        return;
    }
    auto const priority_order = std::ranges::find_if(
        m_height_tile_interest_orders->orders,
        [heading](shared::HeightTileInterest const& order) {
            return order.heading_x == heading.x && order.heading_y == heading.y;
        }
    );
    ASSERT(priority_order != m_height_tile_interest_orders->orders.end(), "invalid height-tile heading");
    bool const had_center = stream.has_center;
    bool const center_changed = !had_center || stream.center != next_center;
    bool const heading_changed = !had_center
        || stream.applied_heading_x != heading.x
        || stream.applied_heading_y != heading.y;
    shared::HeightTileInterestDelta delta;
    if (had_center && center_changed) {
        delta = shared::heightTileInterestDelta(stream.center, next_center, stream.render_distance);
    }
    stream.center = next_center;
    stream.has_center = true;
    stream.applied_heading_x = heading.x;
    stream.applied_heading_y = heading.y;
    stream.priority_offsets = priority_order->keys;
    stream.priority_cursor_generation = stream.generation;
    if (heading_changed) {
        stream.priority_cursor = 0U;
    }
    if (center_changed) {
        for (shared::HeightTileKey const key : delta.additions) {
            stream.pending_removals.erase(key);
        }
        for (shared::HeightTileKey const key : delta.removals) {
            if (stream.resident_keys.contains(key) && !stream.inflight_removal_keys.contains(key)) {
                stream.pending_removals.insert(key);
            }
        }
        stream.urgent_keys = std::move(delta.additions);
        stream.urgent_cursor = 0U;
        if (stream.urgent_keys.size() > PreviewStream::MAX_URGENT_CANDIDATES_PER_PUMP * 8U) {
            stream.urgent_keys.clear();
            stream.priority_cursor = 0U;
        } else {
            std::ranges::sort(stream.urgent_keys, [&stream](
                shared::HeightTileKey const first,
                shared::HeightTileKey const second
            ) {
                return heightTilePriority(
                    stream.center, stream.heading_x, stream.heading_y, first
                ) < heightTilePriority(
                    stream.center, stream.heading_x, stream.heading_y, second
                );
            });
        }
        auto const now = std::chrono::steady_clock::now();
        stream.background_generation_tokens = 0.0;
        stream.background_budget_updated_at = now;
        stream.background_generation_eligible_at = now + std::chrono::milliseconds{250};
    }

    std::erase_if(stream.queued_keys, [&stream](shared::HeightTileKey const key) {
        return !stream.desires(key);
    });
    stream.scheduler.cancelQueuedIf([&stream](shared::GenerationJob const& job) {
        return !stream.desires({.x = job.coordinate.x, .y = job.coordinate.y});
    });
    for (shared::GenerationJob const& job : m_height_tile_workers->cancelQueuedIf(
             stream.client_id,
             [&stream](HeightTileWorkerPool::Work const& work) {
                 return !stream.desires({
                     .x = work.job.coordinate.x,
                     .y = work.job.coordinate.y,
                 });
             }
         )) {
        stream.dispatched_keys.erase({.x = job.coordinate.x, .y = job.coordinate.y});
        static_cast<void>(stream.scheduler.complete(job.id, false, true));
    }
    auto const order_jobs = [&stream](shared::GenerationJob const& first, shared::GenerationJob const& second) {
        return heightTilePriority(
            stream.center,
            stream.heading_x,
            stream.heading_y,
            {.x = first.coordinate.x, .y = first.coordinate.y}
        ) < heightTilePriority(
            stream.center,
            stream.heading_x,
            stream.heading_y,
            {.x = second.coordinate.x, .y = second.coordinate.y}
        );
    };
    stream.scheduler.reorderQueued(order_jobs);
    m_height_tile_workers->reorderQueued(stream.client_id, [&order_jobs](
        HeightTileWorkerPool::Work const& first,
        HeightTileWorkerPool::Work const& second
    ) {
        return order_jobs(first.job, second.job);
    });

    std::erase_if(stream.ready_tiles, [&stream](auto const& entry) {
        return !stream.desires(entry.first);
    });
    fillHeightTileQueue(stream);
}

void GameServer::fillHeightTileQueue(PreviewStream& stream)
{
    if (stream.priority_cursor_generation != stream.generation) {
        stream.priority_cursor = 0U;
        stream.priority_cursor_generation = stream.generation;
    }
    if (stream.background_cursor_generation != stream.generation) {
        stream.background_cursor = 0U;
        stream.background_cursor_generation = stream.generation;
    }
    if (stream.scheduler.pendingCount() >= PreviewStream::MAX_QUEUED_TILES
        || stream.priority_offsets.empty()) {
        return;
    }
    auto const enqueue = [&stream](shared::HeightTileKey const key, uint32_t& submissions) {
        if (stream.resident_keys.contains(key)
            || stream.queued_keys.contains(key)
            || stream.dispatched_keys.contains(key)
            || stream.ready_tiles.contains(key)
            || stream.inflight_addition_keys.contains(key)) {
            return true;
        }
        shared::GenerationAdmission const admission = stream.scheduler.submit(
            {.x = key.x, .y = key.y, .z = 0},
            stream.generation,
            shared::GenerationStage::HeightTile
        );
        if (admission == shared::GenerationAdmission::QueueFull) {
            return false;
        }
        if (admission == shared::GenerationAdmission::Accepted) {
            stream.queued_keys.insert(key);
            ++submissions;
        }
        return true;
    };
    uint32_t const initial_slots = PreviewStream::MAX_QUEUED_TILES
        - static_cast<uint32_t>(stream.scheduler.pendingCount());
    uint32_t const urgent_submission_limit = stream.urgent_cursor < stream.urgent_keys.size()
        ? std::min<uint32_t>(64U, std::max<uint32_t>(1U, initial_slots / 2U))
        : 0U;
    uint32_t urgent_submissions = 0U;
    uint32_t urgent_inspections = 0U;
    while (stream.urgent_cursor < stream.urgent_keys.size()
        && urgent_inspections < PreviewStream::MAX_URGENT_CANDIDATES_PER_PUMP
        && urgent_submissions < urgent_submission_limit
        && stream.scheduler.pendingCount() < PreviewStream::MAX_QUEUED_TILES) {
        shared::HeightTileKey const key = stream.urgent_keys[stream.urgent_cursor++];
        ++urgent_inspections;
        if (stream.desires(key) && !enqueue(key, urgent_submissions)) {
            return;
        }
    }
    if (stream.urgent_cursor >= stream.urgent_keys.size()) {
        stream.urgent_keys.clear();
        stream.urgent_cursor = 0U;
    }
    uint32_t const offset_count = static_cast<uint32_t>(stream.priority_offsets.size());
    uint32_t const slots_after_urgent = PreviewStream::MAX_QUEUED_TILES
        - static_cast<uint32_t>(stream.scheduler.pendingCount());
    uint32_t const priority_submission_limit = slots_after_urgent > 0U
        ? std::min<uint32_t>(32U, std::max<uint32_t>(1U, slots_after_urgent / 2U))
        : 0U;
    uint32_t priority_submissions = 0U;
    for (uint32_t inspected = 0U;
         inspected < PreviewStream::MAX_PRIORITY_CANDIDATES_PER_PUMP
             && priority_submissions < priority_submission_limit
             && stream.scheduler.pendingCount() < PreviewStream::MAX_QUEUED_TILES;
         ++inspected) {
        if (stream.priority_cursor >= offset_count) {
            stream.priority_cursor = 0U;
        }
        shared::HeightTileKey const offset = stream.priority_offsets[stream.priority_cursor++];
        shared::HeightTileKey const key = shared::normalizeHeightTileKey({
            .x = stream.center.x + offset.x,
            .y = stream.center.y + offset.y,
        });
        if (!enqueue(key, priority_submissions)) {
            return;
        }
    }
    uint32_t background_submissions = 0U;
    for (uint32_t inspected = 0U;
         inspected < PreviewStream::MAX_BACKGROUND_CANDIDATES_PER_PUMP
             && stream.scheduler.pendingCount() < PreviewStream::MAX_QUEUED_TILES;
         ++inspected) {
        if (stream.background_cursor >= offset_count) {
            stream.background_cursor = 0U;
        }
        shared::HeightTileKey const offset = stream.priority_offsets[stream.background_cursor++];
        shared::HeightTileKey const key = shared::normalizeHeightTileKey({
            .x = stream.center.x + offset.x,
            .y = stream.center.y + offset.y,
        });
        if (!enqueue(key, background_submissions)) {
            return;
        }
    }
}

void GameServer::processHeightTileStreams()
{
    auto const started_at = m_benchmark_metrics_enabled
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (PreviewStream& stream : m_preview_streams) {
        auto const player = m_world.player(stream.client_id);
        if (player) {
            refreshHeightTileInterest(stream, *player);
        }
    }

    publishHeightTileResults();
    for (PreviewStream& stream : m_preview_streams) {
        fillHeightTileQueue(stream);
    }
    if (!m_preview_streams.empty()) {
        static constexpr uint32_t MAX_ADMITTED_BATCHES_PER_PUMP = 4U;
        uint32_t remaining_batches = MAX_ADMITTED_BATCHES_PER_PUMP;
        size_t const stream_count = m_preview_streams.size();
        size_t round_start = m_next_preview_admission % stream_count;
        while (remaining_batches > 0U) {
            bool admitted = false;
            for (size_t offset = 0U; offset < stream_count && remaining_batches > 0U; ++offset) {
                size_t const index = (round_start + offset) % stream_count;
                uint32_t const admitted_batches = admitHeightTileDeliveries(
                    m_preview_streams[index], 1U
                );
                remaining_batches -= admitted_batches;
                admitted = admitted || admitted_batches != 0U;
            }
            if (!admitted) {
                break;
            }
            round_start = (round_start + 1U) % stream_count;
        }
        m_next_preview_admission = (m_next_preview_admission + 1U) % stream_count;
    }
    dispatchWorldMaterialization();
    dispatchHeightTileWork();
    if (m_benchmark_metrics_enabled) {
        m_last_stream_pump_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started_at
        );
    }
}

void GameServer::dispatchWorldMaterialization()
{
    static constexpr uint32_t MAX_DISPATCHED_PER_PUMP = 16U;
    uint32_t dispatched = 0U;
    while (dispatched < MAX_DISPATCHED_PER_PUMP && m_height_tile_workers->canAccept()) {
        auto const job = m_world_generation.takeNext();
        if (!job.has_value()) {
            break;
        }
        if (!m_height_tile_workers->enqueue({
                .client_id = 0U,
                .generation = job->revision,
                .job = *job,
                .world_generation = true,
            })) {
            static_cast<void>(m_world_generation.complete(job->id, false));
            static_cast<void>(m_world_generation.takeResult());
            break;
        }
        ++dispatched;
    }
}

void GameServer::dispatchHeightTileWork()
{
    static constexpr uint32_t MAX_DISPATCHED_TILES_PER_PUMP = PreviewStream::MAX_QUEUED_TILES;
    static constexpr double BACKGROUND_TILES_PER_SECOND = 8'192.0;
    static constexpr double MAXIMUM_BACKGROUND_BURST = 128.0;
    auto const now = std::chrono::steady_clock::now();
    for (PreviewStream& stream : m_preview_streams) {
        double const elapsed_seconds = now >= stream.background_generation_eligible_at
            ? std::chrono::duration<double>(now - std::max(
                stream.background_budget_updated_at,
                stream.background_generation_eligible_at
            )).count()
            : 0.0;
        stream.background_budget_updated_at = now;
        stream.background_generation_tokens = std::min(
            MAXIMUM_BACKGROUND_BURST,
            stream.background_generation_tokens + elapsed_seconds * BACKGROUND_TILES_PER_SECOND
        );
    }
    if (m_preview_streams.empty()) {
        return;
    }

    uint32_t dispatched = 0U;
    size_t const stream_count = m_preview_streams.size();
    size_t round_start = m_next_preview_dispatch % stream_count;
    while (dispatched < MAX_DISPATCHED_TILES_PER_PUMP && m_height_tile_workers->canAccept()) {
        bool dispatched_round = false;
        for (size_t offset = 0U; offset < stream_count
            && dispatched < MAX_DISPATCHED_TILES_PER_PUMP
            && m_height_tile_workers->canAccept(); ++offset) {
            PreviewStream& stream = m_preview_streams[(round_start + offset) % stream_count];
            if (stream.ready_tiles.size() + stream.dispatched_keys.size() >= PreviewStream::MAX_BUFFERED_TILES) {
                continue;
            }
            auto const next = stream.scheduler.peekNext();
            if (!next.has_value()) {
                continue;
            }
            bool const background = shared::heightTileGenerationBand(
                stream.center,
                stream.heading_x,
                stream.heading_y,
                {.x = next->coordinate.x, .y = next->coordinate.y}
            ) == shared::HeightTileGenerationBand::Background;
            if (background && stream.background_generation_tokens < 1.0) {
                continue;
            }
            auto const job = stream.scheduler.takeNext();
            if (!job) {
                continue;
            }
            shared::HeightTileKey const key{.x = job->coordinate.x, .y = job->coordinate.y};
            stream.queued_keys.erase(key);
            if (!m_height_tile_workers->enqueue({
                .client_id = stream.client_id,
                .generation = stream.generation,
                .job = *job,
            })) {
                static_cast<void>(stream.scheduler.complete(job->id, false, true));
                stream.priority_cursor = 0U;
                break;
            }
            stream.dispatched_keys.insert(key);
            if (background) {
                stream.background_generation_tokens -= 1.0;
            }
            ++dispatched;
            dispatched_round = true;
        }
        if (!dispatched_round) {
            break;
        }
        round_start = (round_start + 1U) % stream_count;
    }
    m_next_preview_dispatch = (m_next_preview_dispatch + 1U) % stream_count;
}

void GameServer::publishHeightTileResults()
{
    static constexpr uint32_t MAX_PUBLISHED_RESULTS_PER_PUMP = 4U * shared::HEIGHT_TILE_BATCH_CAPACITY;
    std::vector<HeightTileWorkerPool::Result> results = m_height_tile_workers->takeResults(
        MAX_PUBLISHED_RESULTS_PER_PUMP
    );
    std::ranges::stable_sort(results, [this](
        HeightTileWorkerPool::Result const& first,
        HeightTileWorkerPool::Result const& second
    ) {
        if (first.world_generation != second.world_generation) {
            return first.world_generation;
        }
        if (first.world_generation) {
            return std::tie(first.job.revision, first.job.coordinate.z, first.job.coordinate.y,
                first.job.coordinate.x, first.job.id)
                < std::tie(second.job.revision, second.job.coordinate.z, second.job.coordinate.y,
                    second.job.coordinate.x, second.job.id);
        }
        if (first.client_id != second.client_id) {
            return first.client_id < second.client_id;
        }
        if (first.generation != second.generation) {
            return first.generation < second.generation;
        }
        auto const stream = std::ranges::find(m_preview_streams, first.client_id, &PreviewStream::client_id);
        if (stream == m_preview_streams.end()) {
            return false;
        }
        auto const priority = [&stream](shared::GenerationJob const& job) {
            return heightTilePriority(
                stream->center,
                stream->heading_x,
                stream->heading_y,
                {.x = job.coordinate.x, .y = job.coordinate.y}
            );
        };
        return priority(first.job) < priority(second.job);
    });
    for (HeightTileWorkerPool::Result& result : results) {
        if (result.world_generation) {
            bool const current_identity = result.job.revision == m_world.configuration().algorithm_version
                && result.job.seed == m_world.configuration().seed;
            if (!current_identity || result.cancelled) {
                static_cast<void>(m_world_generation.complete(result.job.id, false, true));
                continue;
            }
            std::vector<uint8_t> output = result.job.stage == shared::GenerationStage::Pregen
                    || result.job.stage == shared::GenerationStage::Refinement
                ? std::move(result.generation_output)
                : std::vector<uint8_t>{};
            if (!m_world_generation.complete(result.job.id, result.succeeded, false, std::move(output))) {
                continue;
            }
            auto const generated = m_world_generation.takeResult();
            if (!generated.has_value() || generated->job.id != result.job.id || !generated->succeeded) {
                continue;
            }
            if (result.job.stage == shared::GenerationStage::Materialize) {
                if (m_physics_world.publishMaterializedChunk(
                        std::move(result.chunk), result.job.revision, result.job.seed
                    )) {
                    static_cast<void>(m_world_generation.acceptResult(result.job.id));
                } else {
                    static_cast<void>(m_world_generation.cancel(
                        result.job.coordinate, result.job.revision, result.job.seed
                    ));
                }
                continue;
            }
            if (result.job.stage == shared::GenerationStage::HeightTile) {
                if (!m_physics_world.publishPreview(
                        result.job.coordinate,
                        std::move(result.tile),
                        result.job.revision,
                        result.job.seed
                    )) {
                    static_cast<void>(m_world_generation.cancel(
                        result.job.coordinate, result.job.revision, result.job.seed
                    ));
                    continue;
                }
            }
            static_cast<void>(m_world_generation.acceptResult(result.job.id));
            continue;
        }
        auto const stream = std::ranges::find(m_preview_streams, result.client_id, &PreviewStream::client_id);
        if (stream == m_preview_streams.end()) {
            continue;
        }
        if (stream->generation != result.generation || result.job.revision != result.generation) {
            static_cast<void>(stream->scheduler.complete(result.job.id, false, true));
            continue;
        }
        shared::HeightTileKey const key{.x = result.job.coordinate.x, .y = result.job.coordinate.y};
        stream->dispatched_keys.erase(key);
        if (!stream->scheduler.complete(result.job.id, result.succeeded)) {
            continue;
        }
        auto const scheduler_result = stream->scheduler.takeResult();
        if (!scheduler_result || scheduler_result->job.id != result.job.id) {
            continue;
        }
        if (!scheduler_result->succeeded) {
            if (stream->scheduler.retry(result.job.id)) {
                stream->queued_keys.insert(key);
            } else {
                stream->priority_cursor = 0U;
            }
            continue;
        }
        if (!stream->desires(key) || stream->resident_keys.contains(key)
            || stream->inflight_addition_keys.contains(key)) {
            continue;
        }
        shared::ServerHeightTileMessage tile{
            .key = key,
            .revision = PreviewStream::WORLD_REVISION,
            .token = nextHeightTileToken(m_next_height_tile_token),
            .heights = result.tile.heights,
        };
        stream->ready_tiles.insert_or_assign(key, std::move(tile));
    }
}

} // namespace server
