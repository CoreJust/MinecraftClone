#include <shared/world/WorldGenerationScheduler.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace shared {

namespace {

constexpr int64_t WORLD_XY_WIDTH = 65'536;

[[nodiscard]]
bool isRegisteredExtent(uint32_t const extent) noexcept
{
    return std::ranges::find(REGISTERED_WORLD_GENERATION_EXTENTS, extent)
        != REGISTERED_WORLD_GENERATION_EXTENTS.end();
}

[[nodiscard]]
uint32_t priorityScore(
    GenerationPriorityScores const scores,
    GenerationPriorityDimension const dimension
) noexcept
{
    switch (dimension) {
    case GenerationPriorityDimension::Distance:
        return scores.distance;
    case GenerationPriorityDimension::View:
        return scores.view;
    case GenerationPriorityDimension::Movement:
        return scores.movement;
    }
    return std::numeric_limits<uint32_t>::max();
}

[[nodiscard]]
std::vector<GenerationPriorityDimension> normalizedPriorityOrder(
    std::span<GenerationPriorityDimension const> priority_order
)
{
    if (priority_order.empty()) {
        return std::vector<GenerationPriorityDimension>(
            DEFAULT_GENERATION_PRIORITY_ORDER.begin(),
            DEFAULT_GENERATION_PRIORITY_ORDER.end()
        );
    }
    return {priority_order.begin(), priority_order.end()};
}

} // namespace

std::optional<GenerationRegionKey> generationRegionForChunk(
    ChunkCoordinate const coordinate,
    uint32_t const extent,
    GenerationStage const stage,
    uint64_t const revision,
    uint64_t const seed
) noexcept
{
    if (!isRegisteredExtent(extent)
        || (stage != GenerationStage::Pregen && stage != GenerationStage::Refinement)) {
        return std::nullopt;
    }
    auto const canonicalOrigin = [extent](int32_t const chunk_coordinate) {
        int64_t const chunk_offset = static_cast<int64_t>(chunk_coordinate) * Chunk::SIDE_LENGTH;
        int64_t const wrapped = (chunk_offset % WORLD_XY_WIDTH + WORLD_XY_WIDTH) % WORLD_XY_WIDTH;
        return static_cast<int32_t>((wrapped / static_cast<int64_t>(extent)) * extent);
    };
    return GenerationRegionKey{
        .revision = revision,
        .seed = seed,
        .origin_x = canonicalOrigin(coordinate.x),
        .origin_y = canonicalOrigin(coordinate.y),
        .extent = extent,
        .stage = stage,
    };
}

bool isValidWorldGenerationPlan(WorldGenerationPlan const& plan) noexcept
{
    if (plan.refinement_extents.size() > REGISTERED_WORLD_GENERATION_EXTENTS.size()
        || plan.priority_order.size() > DEFAULT_GENERATION_PRIORITY_ORDER.size()) {
        return false;
    }
    std::array<bool, 3> seen_priorities{};
    for (GenerationPriorityDimension const dimension : plan.priority_order) {
        size_t const index = static_cast<size_t>(dimension);
        if (index >= seen_priorities.size() || seen_priorities[index]) {
            return false;
        }
        seen_priorities[index] = true;
    }

    if (plan.pre_generate && !plan.refinement_extents.empty()
        && plan.refinement_extents.front() == REGISTERED_WORLD_GENERATION_EXTENTS.front()) {
        return false;
    }
    uint32_t previous_extent = plan.pre_generate
        ? REGISTERED_WORLD_GENERATION_EXTENTS.front()
        : std::numeric_limits<uint32_t>::max();
    for (uint32_t const extent : plan.refinement_extents) {
        if (!isRegisteredExtent(extent) || extent >= previous_extent) {
            return false;
        }
        previous_extent = extent;
    }

    bool has_height_tile = false;
    bool has_materialize = false;
    for (size_t index = 0U; index < plan.chunk_stages.size(); ++index) {
        switch (plan.chunk_stages[index]) {
        case GenerationStage::HeightTile:
            if (has_height_tile) {
                return false;
            }
            has_height_tile = true;
            break;
        case GenerationStage::Materialize:
            if (has_materialize || index + 1U != plan.chunk_stages.size()) {
                return false;
            }
            has_materialize = true;
            break;
        default:
            return false;
        }
    }
    return plan.pre_generate || !plan.refinement_extents.empty() || !plan.chunk_stages.empty();
}

WorldGenerationScheduler::WorldGenerationScheduler(
    uint64_t const max_pending_jobs,
    uint64_t const max_output_bytes
)
    : m_max_pending_jobs(max_pending_jobs)
    , m_max_output_bytes(max_output_bytes)
{
    if (m_max_pending_jobs == 0U || m_max_output_bytes == 0U) {
        throw std::invalid_argument{"world generation scheduler requires positive work and output bounds"};
    }
    m_jobs.reserve(static_cast<decltype(m_jobs)::size_type>(max_pending_jobs));
}

GenerationAdmission WorldGenerationScheduler::submit(
    ChunkCoordinate const coordinate,
    uint64_t const revision,
    GenerationStage const stage,
    GenerationPriorityScores const priority_scores
)
{
    return submitJob(coordinate, revision, 0U, stage, priority_scores).admission;
}

WorldGenerationScheduler::AdmissionResult WorldGenerationScheduler::submitJob(
    ChunkCoordinate const coordinate,
    uint64_t const revision,
    uint64_t const seed,
    GenerationStage const stage,
    GenerationPriorityScores const priority_scores,
    std::optional<GenerationRegionKey> region,
    std::shared_ptr<std::vector<uint8_t> const> inherited_ancestor_data
)
{
    for (auto const& [id, state] : m_jobs) {
        static_cast<void>(id);
        bool const same_identity = region.has_value()
            ? state.job.region == region
            : !state.job.region.has_value() && state.job.coordinate == coordinate;
        if (same_identity
            && state.job.revision == revision
            && state.job.seed == seed
            && state.job.stage == stage
            && !state.stale
            && !state.cancelled) {
            return {
                .admission = GenerationAdmission::Duplicate,
            };
        }
    }
    if (m_jobs.size() >= m_max_pending_jobs) {
        return {
            .admission = GenerationAdmission::QueueFull,
        };
    }
    GenerationJob const job{
        .id = m_next_id++,
        .coordinate = coordinate,
        .revision = revision,
        .seed = seed,
        .stage = stage,
        .priority_scores = priority_scores,
        .region = std::move(region),
        .inherited_ancestor_data = std::move(inherited_ancestor_data),
    };
    m_jobs.emplace(job.id, JobState{.job = job});
    m_queue.push_back(job.id);
    return {
        .admission = GenerationAdmission::Accepted,
        .job = job,
    };
}

std::optional<GenerationJob> WorldGenerationScheduler::takeNext() noexcept
{
    while (!m_queue.empty()) {
        GenerationJobId const id = m_queue.front();
        m_queue.pop_front();
        auto const iterator = m_jobs.find(id);
        if (iterator == m_jobs.end()) {
            continue;
        }
        if (iterator->second.stale || iterator->second.cancelled
            || iterator->second.running || iterator->second.job.retries > MAX_RETRIES) {
            if (!iterator->second.running) {
                m_jobs.erase(iterator);
            }
            continue;
        }
        iterator->second.running = true;
        return iterator->second.job;
    }
    return std::nullopt;
}

std::optional<GenerationJob> WorldGenerationScheduler::peekNext() const noexcept
{
    for (GenerationJobId const id : m_queue) {
        auto const iterator = m_jobs.find(id);
        if (iterator != m_jobs.end() && !iterator->second.stale && !iterator->second.cancelled
            && !iterator->second.running && iterator->second.job.retries <= MAX_RETRIES) {
            return iterator->second.job;
        }
    }
    return std::nullopt;
}

bool WorldGenerationScheduler::complete(
    GenerationJobId const id,
    bool const succeeded,
    bool const cancelled,
    std::vector<uint8_t> output
)
{
    auto const iterator = m_jobs.find(id);
    if (iterator == m_jobs.end() || !iterator->second.running) {
        return false;
    }
    JobState& state = iterator->second;
    state.running = false;
    state.completed = true;
    if (state.stale || state.cancelled || cancelled) {
        state.cancelled = true;
        m_jobs.erase(iterator);
        return true;
    }
    bool completion_succeeded = succeeded;
    if (output.size() > MAX_GENERATION_STAGE_OUTPUT_BYTES
        || output.size() > m_max_output_bytes - std::min(m_output_bytes, m_max_output_bytes)) {
        completion_succeeded = false;
        output.clear();
    }
    state.succeeded = completion_succeeded;
    if (m_results.size() < m_max_pending_jobs) {
        std::shared_ptr<std::vector<uint8_t> const> immutable_output;
        if (completion_succeeded && !output.empty()) {
            immutable_output = std::make_shared<std::vector<uint8_t> const>(std::move(output));
            m_output_bytes += static_cast<uint64_t>(immutable_output->size());
        }
        m_results.push_back(GenerationResult{
            .job = state.job,
            .succeeded = completion_succeeded,
            .cancelled = false,
            .output = std::move(immutable_output),
        });
    } else {
        state.cancelled = true;
        m_jobs.erase(iterator);
    }
    return true;
}

bool WorldGenerationScheduler::retry(GenerationJobId const id) noexcept
{
    auto const iterator = m_jobs.find(id);
    if (iterator == m_jobs.end()) {
        return false;
    }
    JobState& state = iterator->second;
    if (state.running || !state.completed || !state.result_taken || state.succeeded || state.stale || state.cancelled
        || state.job.retries >= MAX_RETRIES
        || m_queue.size() >= m_max_pending_jobs) {
        return false;
    }
    state.job.retries = static_cast<uint8_t>(state.job.retries + 1U);
    state.running = false;
    state.completed = false;
    state.result_taken = false;
    m_queue.push_back(id);
    return true;
}

bool WorldGenerationScheduler::cancel(GenerationJobId const id) noexcept
{
    auto const iterator = m_jobs.find(id);
    if (iterator == m_jobs.end() || iterator->second.cancelled) {
        return false;
    }
    if (!iterator->second.running) {
        auto const queued = std::find(m_queue.begin(), m_queue.end(), id);
        if (queued != m_queue.end()) {
            m_queue.erase(queued);
        }
        std::erase_if(m_results, [this, id](GenerationResult const& result) {
            if (result.job.id != id) {
                return false;
            }
            if (result.output != nullptr) {
                m_output_bytes -= static_cast<uint64_t>(result.output->size());
            }
            return true;
        });
        m_jobs.erase(iterator);
    } else {
        iterator->second.cancelled = true;
    }
    return true;
}

void WorldGenerationScheduler::cancelQueued() noexcept
{
    for (GenerationJobId const id : m_queue) {
        m_jobs.erase(id);
    }
    m_queue.clear();
}

void WorldGenerationScheduler::cancelQueuedIf(std::function<bool(GenerationJob const&)> const& should_cancel)
{
    auto queued = m_queue.begin();
    while (queued != m_queue.end()) {
        auto const state = m_jobs.find(*queued);
        if (state == m_jobs.end() || should_cancel(state->second.job)) {
            if (state != m_jobs.end()) {
                m_jobs.erase(state);
            }
            queued = m_queue.erase(queued);
        } else {
            ++queued;
        }
    }
}

void WorldGenerationScheduler::reorderQueued(JobOrder const& order)
{
    std::stable_sort(m_queue.begin(), m_queue.end(), [this, &order](GenerationJobId const first, GenerationJobId const second) {
        auto const first_state = m_jobs.find(first);
        auto const second_state = m_jobs.find(second);
        return first_state != m_jobs.end()
            && second_state != m_jobs.end()
            && order(first_state->second.job, second_state->second.job);
    });
}

bool WorldGenerationScheduler::reorderQueued(
    std::span<GenerationPriorityDimension const> priority_order
)
{
    if (priority_order.size() > DEFAULT_GENERATION_PRIORITY_ORDER.size()) {
        return false;
    }
    std::array<bool, DEFAULT_GENERATION_PRIORITY_ORDER.size()> seen{};
    for (GenerationPriorityDimension const dimension : priority_order) {
        size_t const index = static_cast<size_t>(dimension);
        if (index >= seen.size() || seen[index]) {
            return false;
        }
        seen[index] = true;
    }
    std::vector<GenerationPriorityDimension> const order = normalizedPriorityOrder(priority_order);
    std::stable_sort(m_queue.begin(), m_queue.end(), [this, &order](GenerationJobId const first, GenerationJobId const second) {
        auto const first_state = m_jobs.find(first);
        auto const second_state = m_jobs.find(second);
        if (first_state == m_jobs.end() || second_state == m_jobs.end()) {
            return first_state != m_jobs.end();
        }
        GenerationJob const& first_job = first_state->second.job;
        GenerationJob const& second_job = second_state->second.job;
        for (GenerationPriorityDimension const dimension : order) {
            uint32_t const first_score = priorityScore(first_job.priority_scores, dimension);
            uint32_t const second_score = priorityScore(second_job.priority_scores, dimension);
            if (first_score != second_score) {
                return first_score < second_score;
            }
        }
        auto const region_key = [](GenerationJob const& job) {
            return job.region.has_value()
                ? std::tuple{
                    job.region->extent,
                    job.region->origin_y,
                    job.region->origin_x,
                    static_cast<uint8_t>(job.region->stage),
                }
                : std::tuple{0U, 0, 0, static_cast<uint8_t>(job.stage)};
        };
        return std::tuple{
            first_job.revision,
            first_job.seed,
            first_job.coordinate.z,
            first_job.coordinate.y,
            first_job.coordinate.x,
            region_key(first_job),
            first_job.id,
        } < std::tuple{
            second_job.revision,
            second_job.seed,
            second_job.coordinate.z,
            second_job.coordinate.y,
            second_job.coordinate.x,
            region_key(second_job),
            second_job.id,
        };
    });
    return true;
}

void WorldGenerationScheduler::invalidateRevision(uint64_t const revision) noexcept
{
    for (auto& [id, state] : m_jobs) {
        static_cast<void>(id);
        if (state.job.revision == revision) {
            state.stale = true;
        }
    }
}

std::optional<GenerationResult> WorldGenerationScheduler::takeResult() noexcept
{
    while (!m_results.empty()) {
        GenerationResult result = std::move(m_results.front());
        m_results.pop_front();
        if (result.output != nullptr) {
            m_output_bytes -= static_cast<uint64_t>(result.output->size());
        }
        auto const state = m_jobs.find(result.job.id);
        if (state == m_jobs.end()) {
            continue;
        }
        if (state->second.stale || state->second.cancelled || result.cancelled) {
            m_jobs.erase(state);
            continue;
        }
        if (result.succeeded || result.job.retries >= MAX_RETRIES) {
            m_jobs.erase(state);
        } else {
            state->second.result_taken = true;
        }
        return result;
    }
    return std::nullopt;
}

uint64_t WorldGenerationScheduler::pendingCount() const noexcept
{
    return static_cast<uint64_t>(m_queue.size());
}

uint64_t WorldGenerationScheduler::resultCount() const noexcept
{
    return static_cast<uint64_t>(m_results.size());
}

uint64_t WorldGenerationCoordinator::GenerationIdentityHash::operator()(
    GenerationIdentity const& identity
) const noexcept
{
    uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (int32_t const component : {
        identity.coordinate.x,
        identity.coordinate.y,
        identity.coordinate.z,
    }) {
        hash ^= static_cast<uint32_t>(component);
        hash *= 1'099'511'628'211ULL;
    }
    hash ^= identity.revision;
    hash *= 1'099'511'628'211ULL;
    hash ^= identity.seed;
    hash *= 1'099'511'628'211ULL;
    return hash;
}

uint64_t WorldGenerationCoordinator::RefinementOutputHash::operator()(
    GenerationRegionKey const& key
) const noexcept
{
    uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (int32_t const component : {key.origin_x, key.origin_y}) {
        hash ^= static_cast<uint32_t>(component);
        hash *= 1'099'511'628'211ULL;
    }
    hash ^= key.extent;
    hash *= 1'099'511'628'211ULL;
    hash ^= static_cast<uint8_t>(key.stage);
    hash *= 1'099'511'628'211ULL;
    hash ^= key.revision;
    hash *= 1'099'511'628'211ULL;
    hash ^= key.seed;
    hash *= 1'099'511'628'211ULL;
    return hash;
}

WorldGenerationCoordinator::WorldGenerationCoordinator(
    uint64_t const max_pending_jobs,
    uint64_t const revision,
    uint64_t const seed
)
    : m_scheduler{max_pending_jobs}
    , m_max_pending_jobs{max_pending_jobs}
    , m_revision{revision}
    , m_seed{seed}
{
    m_pipelines.reserve(static_cast<decltype(m_pipelines)::size_type>(max_pending_jobs));
    m_refinement_outputs.reserve(static_cast<decltype(m_refinement_outputs)::size_type>(
        std::min<uint64_t>(max_pending_jobs, MAX_GENERATION_OUTPUT_BUDGET_BYTES / MAX_GENERATION_STAGE_OUTPUT_BYTES)
    ));
}

bool WorldGenerationCoordinator::isValidPlan(std::vector<GenerationStage> const& stages) noexcept
{
    if (stages.empty()) {
        return false;
    }
    bool has_height_tile = false;
    bool has_materialize = false;
    for (uint64_t index = 0U; index < stages.size(); ++index) {
        switch (stages[static_cast<std::vector<GenerationStage>::size_type>(index)]) {
        case GenerationStage::HeightTile:
            if (has_height_tile) {
                return false;
            }
            has_height_tile = true;
            break;
        case GenerationStage::Materialize:
            if (has_materialize || index + 1U != stages.size()) {
                return false;
            }
            has_materialize = true;
            break;
        default:
            return false;
        }
    }
    return true;
}

GenerationAdmission WorldGenerationCoordinator::request(
    ChunkCoordinate const coordinate,
    std::vector<GenerationStage> stages
)
{
    WorldGenerationPlan plan{
        .chunk_stages = std::move(stages),
    };
    return requestPlan(coordinate, std::move(plan));
}

GenerationAdmission WorldGenerationCoordinator::requestPlan(
    ChunkCoordinate const coordinate,
    WorldGenerationPlan plan,
    GenerationPriorityScores const priority_scores
)
{
    if (!isValidWorldGenerationPlan(plan)) {
        return GenerationAdmission::InvalidPlan;
    }
    std::vector<GenerationPriorityDimension> const priority_order = normalizedPriorityOrder(
        plan.priority_order
    );
    if (m_priority_order_configured && priority_order != m_priority_order) {
        return GenerationAdmission::InvalidPlan;
    }
    m_priority_order = priority_order;
    m_priority_order_configured = true;

    GenerationIdentity const identity{
        .coordinate = coordinate,
        .revision = m_revision,
        .seed = m_seed,
    };
    if (auto const existing = m_pipelines.find(identity); existing != m_pipelines.end()) {
        bool const terminal = existing->second.active_job_id == 0U
            && (existing->second.state == GenerationState::Preview
                || existing->second.state == GenerationState::Materialized
                || existing->second.state == GenerationState::Cancelled);
        if (!terminal) {
            return GenerationAdmission::Duplicate;
        }
        m_pipelines.erase(existing);
    }
    pruneTerminalPipelines();
    if (m_pipelines.size() >= m_max_pending_jobs) {
        return GenerationAdmission::QueueFull;
    }

    std::vector<PlanStage> stages;
    stages.reserve(
        plan.refinement_extents.size() + plan.chunk_stages.size() + static_cast<size_t>(plan.pre_generate)
    );
    auto append_region_stage = [&](uint32_t const extent, GenerationStage const stage) {
        std::optional<GenerationRegionKey> const region = generationRegionForChunk(
            coordinate,
            extent,
            stage,
            m_revision,
            m_seed
        );
        if (region.has_value()) {
            stages.push_back(PlanStage{.stage = stage, .region = region});
        }
    };
    if (plan.pre_generate) {
        append_region_stage(REGISTERED_WORLD_GENERATION_EXTENTS.front(), GenerationStage::Pregen);
    }
    for (uint32_t const extent : plan.refinement_extents) {
        append_region_stage(extent, GenerationStage::Refinement);
    }
    for (GenerationStage const stage : plan.chunk_stages) {
        stages.push_back(PlanStage{.stage = stage});
    }

    auto const [pipeline, inserted] = m_pipelines.emplace(identity, Pipeline{
        .stages = std::move(stages),
        .priority_scores = priority_scores,
        .state = GenerationState::Queued,
    });
    if (!inserted) {
        return GenerationAdmission::Duplicate;
    }
    GenerationAdmission const admission = submitNextStage(identity, pipeline->second);
    if (admission != GenerationAdmission::Accepted && admission != GenerationAdmission::QueueFull) {
        m_pipelines.erase(pipeline);
        return admission;
    }
    return GenerationAdmission::Accepted;
}

std::optional<GenerationJob> WorldGenerationCoordinator::takeNext() noexcept
{
    for (auto& [identity, pipeline] : m_pipelines) {
        if (pipeline.state == GenerationState::Queued && pipeline.active_job_id == 0U) {
            static_cast<void>(submitNextStage(identity, pipeline));
        }
    }
    static_cast<void>(m_scheduler.reorderQueued(m_priority_order));
    std::optional<GenerationJob> job = m_scheduler.takeNext();
    if (job.has_value()) {
        Pipeline* const pipeline = pipelineFor(job->id);
        if (pipeline != nullptr) {
            pipeline->state = GenerationState::Running;
        }
    }
    return job;
}

std::optional<GenerationJob> WorldGenerationCoordinator::peekNext() const noexcept
{
    return m_scheduler.peekNext();
}

bool WorldGenerationCoordinator::complete(
    GenerationJobId const id,
    bool const succeeded,
    bool const cancelled,
    std::vector<uint8_t> output
)
{
    if (!m_scheduler.complete(id, succeeded, cancelled, std::move(output))) {
        return false;
    }
    Pipeline* const pipeline = pipelineFor(id);
    if (pipeline != nullptr && cancelled) {
        pipeline->state = GenerationState::Cancelled;
        pipeline->active_job_id = 0U;
        pipeline->result_ready = false;
        pipeline->result_output.reset();
        pipeline->final_refinement_output.reset();
    }
    return true;
}

std::optional<GenerationResult> WorldGenerationCoordinator::takeResult() noexcept
{
    std::optional<GenerationResult> result = m_scheduler.takeResult();
    if (!result.has_value()) {
        return std::nullopt;
    }
    Pipeline* const pipeline = pipelineFor(result->job.id);
    if (pipeline != nullptr) {
        if (result->cancelled) {
            pipeline->state = GenerationState::Cancelled;
            pipeline->active_job_id = 0U;
            pipeline->result_output.reset();
            pipeline->final_refinement_output.reset();
        } else {
            GenerationStage const stage = pipeline->stages[
                static_cast<std::vector<PlanStage>::size_type>(pipeline->stage_index)
            ].stage;
            pipeline->state = result->succeeded && stage == GenerationStage::HeightTile
                ? GenerationState::Preview
                : (result->succeeded ? GenerationState::Running : GenerationState::Failed);
            pipeline->result_ready = true;
            pipeline->result_output = result->output;
        }
    }
    return result;
}

bool WorldGenerationCoordinator::acceptResult(GenerationJobId const id)
{
    Pipeline* const pipeline = pipelineFor(id);
    if (pipeline == nullptr || !pipeline->result_ready || pipeline->active_job_id != id
        || (pipeline->state != GenerationState::Preview && pipeline->state != GenerationState::Running)) {
        return false;
    }
    auto const entry = std::ranges::find_if(m_pipelines, [pipeline](auto const& item) {
        return &item.second == pipeline;
    });
    if (entry == m_pipelines.end()) {
        return false;
    }
    GenerationIdentity const& identity = entry->first;
    if (identity.revision != m_revision || identity.seed != m_seed) {
        pipeline->state = GenerationState::Cancelled;
        pipeline->active_job_id = 0U;
        pipeline->result_ready = false;
        pipeline->result_output.reset();
        pipeline->final_refinement_output.reset();
        return false;
    }

    PlanStage const completed_stage = pipeline->stages[
        static_cast<std::vector<PlanStage>::size_type>(pipeline->stage_index)
    ];
    if (completed_stage.region.has_value() && pipeline->result_output != nullptr) {
        if (completed_stage.stage == GenerationStage::Refinement) {
            pipeline->final_refinement_output = pipeline->result_output;
        }
        retainRefinementOutput(*completed_stage.region, pipeline->result_output);
    }
    pipeline->result_ready = false;
    pipeline->result_output.reset();
    ++pipeline->stage_index;
    pipeline->active_job_id = 0U;
    GenerationAdmission const admission = submitNextStage(identity, *pipeline);
    if (admission == GenerationAdmission::InvalidPlan || admission == GenerationAdmission::Duplicate) {
        pipeline->state = GenerationState::Failed;
        return false;
    }
    return true;
}

bool WorldGenerationCoordinator::retry(GenerationJobId const id) noexcept
{
    Pipeline* const pipeline = pipelineFor(id);
    if (pipeline == nullptr || pipeline->state != GenerationState::Failed || !pipeline->result_ready
        || !m_scheduler.retry(id)) {
        return false;
    }
    pipeline->state = GenerationState::Queued;
    pipeline->result_ready = false;
    pipeline->result_output.reset();
    return true;
}

bool WorldGenerationCoordinator::requestRetry(
    ChunkCoordinate const coordinate,
    uint64_t const revision,
    uint64_t const seed
) noexcept
{
    auto const pipeline = m_pipelines.find(GenerationIdentity{
        .coordinate = coordinate,
        .revision = revision,
        .seed = seed,
    });
    return pipeline != m_pipelines.end()
        && pipeline->second.state == GenerationState::Failed
        && pipeline->second.active_job_id != 0U
        && retry(pipeline->second.active_job_id);
}

bool WorldGenerationCoordinator::cancel(
    ChunkCoordinate const coordinate,
    uint64_t const revision,
    uint64_t const seed
) noexcept
{
    auto const pipeline = m_pipelines.find(GenerationIdentity{
        .coordinate = coordinate,
        .revision = revision,
        .seed = seed,
    });
    if (pipeline == m_pipelines.end() || pipeline->second.active_job_id == 0U) {
        return false;
    }
    static_cast<void>(m_scheduler.cancel(pipeline->second.active_job_id));
    pipeline->second.state = GenerationState::Cancelled;
    pipeline->second.active_job_id = 0U;
    pipeline->second.result_ready = false;
    pipeline->second.result_output.reset();
    pipeline->second.final_refinement_output.reset();
    return true;
}

void WorldGenerationCoordinator::setWorldIdentity(
    uint64_t const revision,
    uint64_t const seed
) noexcept
{
    if (m_revision == revision && m_seed == seed) {
        return;
    }
    for (auto& [identity, pipeline] : m_pipelines) {
        if (identity.revision == revision && identity.seed == seed) {
            continue;
        }
        if (pipeline.active_job_id != 0U) {
            static_cast<void>(m_scheduler.cancel(pipeline.active_job_id));
        }
        pipeline.state = GenerationState::Cancelled;
        pipeline.active_job_id = 0U;
        pipeline.result_ready = false;
        pipeline.result_output.reset();
        pipeline.final_refinement_output.reset();
    }
    m_refinement_outputs.clear();
    m_refinement_output_bytes = 0U;
    m_revision = revision;
    m_seed = seed;
}

GenerationState WorldGenerationCoordinator::state(
    ChunkCoordinate const coordinate,
    uint64_t const revision,
    uint64_t const seed
) const noexcept
{
    auto const pipeline = m_pipelines.find(GenerationIdentity{
        .coordinate = coordinate,
        .revision = revision,
        .seed = seed,
    });
    return pipeline == m_pipelines.end() ? GenerationState::Unknown : pipeline->second.state;
}

uint64_t WorldGenerationCoordinator::pendingCount() const noexcept
{
    return m_scheduler.pendingCount();
}

uint64_t WorldGenerationCoordinator::resultCount() const noexcept
{
    return m_scheduler.resultCount();
}

uint64_t WorldGenerationCoordinator::refinementOutputBytes() const noexcept
{
    return m_refinement_output_bytes;
}

GenerationAdmission WorldGenerationCoordinator::submitNextStage(
    GenerationIdentity const& identity,
    Pipeline& pipeline
)
{
    while (pipeline.stage_index < pipeline.stages.size()) {
        PlanStage const& stage = pipeline.stages[
            static_cast<std::vector<PlanStage>::size_type>(pipeline.stage_index)
        ];
        std::shared_ptr<std::vector<uint8_t> const> inherited_ancestor_data;
        if (stage.region.has_value()) {
            auto const exact = m_refinement_outputs.find(*stage.region);
            if (exact != m_refinement_outputs.end()) {
                exact->second.last_access = ++m_refinement_access_clock;
                if (stage.stage == GenerationStage::Refinement) {
                    pipeline.final_refinement_output = exact->second.bytes;
                }
                ++pipeline.stage_index;
                continue;
            }
            inherited_ancestor_data = nearestCompletedAncestor(*stage.region);
        }
        WorldGenerationScheduler::AdmissionResult const submitted = m_scheduler.submitJob(
            identity.coordinate,
            identity.revision,
            identity.seed,
            stage.stage,
            pipeline.priority_scores,
            stage.region,
            stage.stage == GenerationStage::Materialize
                ? pipeline.final_refinement_output
                : std::move(inherited_ancestor_data)
        );
        if (submitted.admission == GenerationAdmission::QueueFull) {
            pipeline.state = GenerationState::Queued;
            pipeline.active_job_id = 0U;
            return GenerationAdmission::QueueFull;
        }
        if (submitted.admission != GenerationAdmission::Accepted || !submitted.job.has_value()) {
            return submitted.admission;
        }
        pipeline.active_job_id = submitted.job->id;
        pipeline.state = GenerationState::Queued;
        static_cast<void>(m_scheduler.reorderQueued(m_priority_order));
        return GenerationAdmission::Accepted;
    }
    pipeline.active_job_id = 0U;
    pipeline.final_refinement_output.reset();
    pipeline.state = !pipeline.stages.empty()
            && pipeline.stages.back().stage == GenerationStage::Materialize
        ? GenerationState::Materialized
        : GenerationState::Preview;
    return GenerationAdmission::Accepted;
}

std::shared_ptr<std::vector<uint8_t> const> WorldGenerationCoordinator::nearestCompletedAncestor(
    GenerationRegionKey const& region
) noexcept
{
    RefinementOutput* closest = nullptr;
    uint32_t closest_extent = std::numeric_limits<uint32_t>::max();
    for (auto& [candidate, output] : m_refinement_outputs) {
        if (candidate.revision != region.revision || candidate.seed != region.seed
            || candidate.extent <= region.extent
            || candidate.origin_x > region.origin_x || candidate.origin_y > region.origin_y
            || static_cast<uint64_t>(candidate.origin_x) + candidate.extent
                < static_cast<uint64_t>(region.origin_x) + region.extent
            || static_cast<uint64_t>(candidate.origin_y) + candidate.extent
                < static_cast<uint64_t>(region.origin_y) + region.extent) {
            continue;
        }
        if (candidate.extent < closest_extent) {
            closest = &output;
            closest_extent = candidate.extent;
        }
    }
    if (closest == nullptr) {
        return nullptr;
    }
    closest->last_access = ++m_refinement_access_clock;
    return closest->bytes;
}

void WorldGenerationCoordinator::retainRefinementOutput(
    GenerationRegionKey const& region,
    std::shared_ptr<std::vector<uint8_t> const> bytes
)
{
    if (bytes == nullptr || bytes->empty() || bytes->size() > MAX_GENERATION_STAGE_OUTPUT_BYTES) {
        return;
    }
    uint64_t const output_size = static_cast<uint64_t>(bytes->size());
    auto const existing = m_refinement_outputs.find(region);
    if (existing != m_refinement_outputs.end()) {
        m_refinement_output_bytes -= static_cast<uint64_t>(existing->second.bytes->size());
        m_refinement_outputs.erase(existing);
    }
    while (m_refinement_output_bytes + output_size > MAX_GENERATION_OUTPUT_BUDGET_BYTES
        && !m_refinement_outputs.empty()) {
        auto oldest = std::ranges::min_element(m_refinement_outputs, {}, [](auto const& item) {
            return item.second.last_access;
        });
        m_refinement_output_bytes -= static_cast<uint64_t>(oldest->second.bytes->size());
        m_refinement_outputs.erase(oldest);
    }
    if (output_size > MAX_GENERATION_OUTPUT_BUDGET_BYTES) {
        return;
    }
    m_refinement_output_bytes += output_size;
    m_refinement_outputs.emplace(region, RefinementOutput{
        .bytes = std::move(bytes),
        .last_access = ++m_refinement_access_clock,
    });
}

WorldGenerationCoordinator::Pipeline* WorldGenerationCoordinator::pipelineFor(
    GenerationJobId const id
) noexcept
{
    for (auto& [identity, pipeline] : m_pipelines) {
        static_cast<void>(identity);
        if (pipeline.active_job_id == id) {
            return &pipeline;
        }
    }
    return nullptr;
}

void WorldGenerationCoordinator::pruneTerminalPipelines() noexcept
{
    if (m_pipelines.size() < m_max_pending_jobs) {
        return;
    }
    auto pipeline = m_pipelines.begin();
    while (pipeline != m_pipelines.end() && m_pipelines.size() >= m_max_pending_jobs) {
        bool const terminal = pipeline->second.active_job_id == 0U
            && (pipeline->second.state == GenerationState::Preview
                || pipeline->second.state == GenerationState::Materialized
                || pipeline->second.state == GenerationState::Cancelled);
        if (terminal) {
            pipeline = m_pipelines.erase(pipeline);
        } else {
            ++pipeline;
        }
    }
}

} // namespace shared
