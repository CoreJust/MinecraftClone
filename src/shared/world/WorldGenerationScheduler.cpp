#include <shared/world/WorldGenerationScheduler.hpp>

#include <shared/world/Chunk.hpp>
#include <shared/world/TerrainWorld.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace shared {

namespace {

constexpr std::array<uint32_t, 4> REQUIRED_TYPED_REFINEMENT_EXTENTS{
    4'096U,
    1'024U,
    256U,
    64U,
};

static_assert(REGISTERED_WORLD_GENERATION_EXTENTS == TERRAIN_REGION_EXTENTS);

[[nodiscard]]
uint64_t regionalArtifactStorageBytes(std::shared_ptr<RegionalArtifact const> const& artifact) noexcept
{
    if (artifact == nullptr) {
        return 0U;
    }
    return static_cast<uint64_t>(sizeof(RegionalArtifact))
        + static_cast<uint64_t>(artifact->identity().configuration_identity.size())
        + 512U;
}

[[nodiscard]]
constexpr uint64_t maximumRegionalArtifactStorageBytes() noexcept
{
    return static_cast<uint64_t>(sizeof(RegionalArtifact))
        + TERRAIN_CONFIGURATION_IDENTITY_MAX_BYTES
        + 512U;
}

// Each admitted pipeline can pin its complete ancestry and its next reserved result.
constexpr uint64_t MAX_ACTIVE_TERRAIN_PIPELINES = MAX_GENERATION_REGIONAL_RETENTION_BUDGET_BYTES
    / maximumRegionalArtifactStorageBytes() / REGISTERED_WORLD_GENERATION_EXTENTS.size();
static_assert(MAX_ACTIVE_TERRAIN_PIPELINES != 0U);

[[nodiscard]]
bool isRegionalStage(GenerationStage const stage) noexcept
{
    return stage == GenerationStage::Pregen || stage == GenerationStage::Refinement;
}

[[nodiscard]]
bool hasExactTypedRegionalParent(GenerationJob const& job)
{
    if (!isRegionalStage(job.stage) || !job.region.has_value() || !job.terrain_identity.has_value()
        || job.world_epoch == 0U || job.region->revision != job.revision || job.region->seed != job.seed) {
        return false;
    }
    GenerationRegionKey const& region = *job.region;
    std::optional<GenerationRegionKey> const expected_region = generationRegionForChunk(
        job.coordinate,
        region.extent,
        job.stage,
        job.revision,
        job.seed
    );
    if (!expected_region.has_value() || *expected_region != region) {
        return false;
    }

    auto const extent = std::ranges::find(REGISTERED_WORLD_GENERATION_EXTENTS, region.extent);
    if (extent == REGISTERED_WORLD_GENERATION_EXTENTS.end()) {
        return false;
    }
    if (extent == REGISTERED_WORLD_GENERATION_EXTENTS.begin()) {
        return job.stage == GenerationStage::Pregen && job.inherited_region == nullptr;
    }
    if (job.stage != GenerationStage::Refinement || job.inherited_region == nullptr) {
        return false;
    }

    uint32_t const parent_extent = *std::prev(extent);
    RegionalArtifact const& parent = *job.inherited_region;
    return parent.identity() == *job.terrain_identity
        && parent.key().extent == parent_extent
        && parent.key().origin_x
            == (static_cast<uint32_t>(region.origin_x) / parent_extent) * parent_extent
        && parent.key().origin_y
            == (static_cast<uint32_t>(region.origin_y) / parent_extent) * parent_extent;
}

// Preview work is admitted before its immutable world and leaf lease are attached.
[[nodiscard]]
bool isValidPreviewContextBinding(GenerationJob const& submitted, GenerationJob const& completed)
{
    if (submitted.stage != GenerationStage::HeightTile || submitted.region.has_value()
        || submitted.terrain_identity.has_value() || submitted.world_epoch != 0U || submitted.seed != 0U
        || submitted.inherited_region != nullptr || submitted.inherited_ancestor_data != nullptr) {
        return false;
    }
    if (completed.stage != GenerationStage::HeightTile || completed.region.has_value()
        || !completed.terrain_identity.has_value() || completed.world_epoch == 0U
        || completed.seed != completed.terrain_identity->seed || completed.inherited_region == nullptr
        || completed.inherited_ancestor_data != nullptr
        || completed.inherited_region->identity() != *completed.terrain_identity
        || completed.inherited_region->key().extent != REGISTERED_WORLD_GENERATION_EXTENTS.back()) {
        return false;
    }
    std::optional<GenerationRegionKey> const expected_leaf = generationRegionForChunk(
        completed.coordinate,
        REGISTERED_WORLD_GENERATION_EXTENTS.back(),
        GenerationStage::Refinement,
        completed.terrain_identity->revision,
        completed.terrain_identity->seed
    );
    return expected_leaf.has_value()
        && completed.inherited_region->key().origin_x == static_cast<uint32_t>(expected_leaf->origin_x)
        && completed.inherited_region->key().origin_y == static_cast<uint32_t>(expected_leaf->origin_y);
}

[[nodiscard]]
bool sameJobAttempt(GenerationJob const& submitted, GenerationJob const& completed)
{
    bool const same_context = (submitted.terrain_identity == completed.terrain_identity
            && submitted.world_epoch == completed.world_epoch
            && submitted.seed == completed.seed
            && submitted.inherited_region == completed.inherited_region)
        || isValidPreviewContextBinding(submitted, completed);
    return submitted.id == completed.id
        && submitted.coordinate == completed.coordinate
        && submitted.revision == completed.revision
        && submitted.stage == completed.stage
        && submitted.retries == completed.retries
        && submitted.region == completed.region
        && submitted.attempt == completed.attempt
        && submitted.inherited_ancestor_data == completed.inherited_ancestor_data
        && submitted.cancellation_source == completed.cancellation_source
        && same_context;
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

WorldGenerationScheduler::WorldGenerationScheduler(
    uint64_t const max_pending_jobs, // NOLINT(bugprone-easily-swappable-parameters) -- job count vs byte limit
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
    std::shared_ptr<std::vector<uint8_t> const> inherited_ancestor_data,
    std::optional<TerrainIdentity> const& terrain_identity,
    SubmissionAttempt const& submission_attempt,
    std::shared_ptr<RegionalArtifact const> const& inherited_region
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
            && state.job.terrain_identity == terrain_identity
            && state.job.world_epoch == submission_attempt.world_epoch
            && !state.stale
            && !state.cancelled) {
            return {
                .admission = GenerationAdmission::Duplicate,
                .job = std::nullopt,
            };
        }
    }
    if (m_jobs.size() >= m_max_pending_jobs) {
        return {
            .admission = GenerationAdmission::QueueFull,
            .job = std::nullopt,
        };
    }
    uint64_t const accepted_attempt = submission_attempt.attempt == 0U
        ? m_next_attempt++
        : submission_attempt.attempt;
    if (accepted_attempt >= m_next_attempt && accepted_attempt != std::numeric_limits<uint64_t>::max()) {
        m_next_attempt = accepted_attempt + 1U;
    }
    GenerationJob const job{
        .id = m_next_id++,
        .coordinate = coordinate,
        .revision = revision,
        .seed = seed,
        .stage = stage,
        .priority_scores = priority_scores,
        .region = region,
        .inherited_ancestor_data = std::move(inherited_ancestor_data),
        .terrain_identity = terrain_identity,
        .world_epoch = submission_attempt.world_epoch,
        .attempt = accepted_attempt,
        .inherited_region = inherited_region,
        .cancellation_source = std::make_shared<std::stop_source>(),
    };
    m_jobs.emplace(job.id, JobState{.job = job});
    m_queue.push_back(job.id);
    return {
        .admission = GenerationAdmission::Accepted,
        .job = job,
    };
}

bool WorldGenerationScheduler::updatePriority(
    GenerationJobId const id,
    GenerationPriorityScores const priority_scores
)
{
    auto const state = m_jobs.find(id);
    if (state == m_jobs.end() || state->second.running || state->second.completed
        || state->second.cancelled || state->second.stale) {
        return false;
    }
    state->second.job.priority_scores = priority_scores;
    return true;
}

bool WorldGenerationScheduler::complete(
    GenerationJob const& completed_job,
    bool const succeeded,
    bool const cancelled,
    std::vector<uint8_t> output
)
{
    auto const state = m_jobs.find(completed_job.id);
    if (state == m_jobs.end() || !state->second.running
        || (state->second.job.terrain_identity.has_value() && isRegionalStage(state->second.job.stage))
        || !sameJobAttempt(state->second.job, completed_job)) {
        return false;
    }
    return complete(completed_job.id, succeeded, cancelled, std::move(output));
}

bool WorldGenerationScheduler::completeRegional(
    GenerationJob const& completed_job,
    bool const succeeded,
    bool const cancelled,
    std::shared_ptr<RegionalArtifact const> output
)
{
    auto const state = m_jobs.find(completed_job.id);
    if (state == m_jobs.end() || !state->second.running
        || !sameJobAttempt(state->second.job, completed_job)
        || !isRegionalStage(completed_job.stage)
        || !hasExactTypedRegionalParent(completed_job)) {
        return false;
    }

    bool valid_output = !succeeded;
    if (succeeded && !cancelled && output != nullptr
        && completed_job.region.has_value() && completed_job.terrain_identity.has_value()) {
        GenerationRegionKey const& expected_region = *completed_job.region;
        TerrainRegionKey const actual_region = output->key();
        valid_output = actual_region
                == TerrainRegionKey{
                    .origin_x = static_cast<uint32_t>(expected_region.origin_x),
                    .origin_y = static_cast<uint32_t>(expected_region.origin_y),
                    .extent = expected_region.extent,
                }
            && output->identity() == *completed_job.terrain_identity
            && output->identity().revision == completed_job.revision
            && output->identity().seed == completed_job.seed
            && output->parent() == completed_job.inherited_region
            && ((completed_job.stage == GenerationStage::Pregen)
                == (completed_job.inherited_region == nullptr));
    }
    if (!succeeded || cancelled) {
        output.reset();
    }
    uint64_t output_bytes = valid_output ? regionalArtifactStorageBytes(output) : 0U;
    if (output_bytes > MAX_GENERATION_REGIONAL_RESULT_BUDGET_BYTES
        - std::min(m_regional_result_bytes, MAX_GENERATION_REGIONAL_RESULT_BUDGET_BYTES)) {
        valid_output = false;
        output.reset();
        output_bytes = 0U;
    }

    JobState& job_state = state->second;
    job_state.running = false;
    job_state.completed = true;
    if (job_state.stale || job_state.cancelled || cancelled) {
        job_state.cancelled = true;
        m_jobs.erase(state);
        return true;
    }
    bool const completion_succeeded = succeeded && valid_output;
    job_state.succeeded = completion_succeeded;
    if (m_results.size() >= m_max_pending_jobs) {
        job_state.cancelled = true;
        m_jobs.erase(state);
        return false;
    }
    m_regional_result_bytes += output_bytes;
    m_results.push_back(GenerationResult{
        .job = job_state.job,
        .succeeded = completion_succeeded,
        .cancelled = false,
        .output = nullptr,
        .regional_artifact = completion_succeeded ? std::move(output) : nullptr,
    });
    return true;
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
    if (iterator == m_jobs.end() || !iterator->second.running
        || (iterator->second.job.terrain_identity.has_value() && isRegionalStage(iterator->second.job.stage))) {
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

bool WorldGenerationScheduler::retry(GenerationJobId const id)
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
    uint64_t const next_attempt = std::max(m_next_attempt, state.job.attempt + 1U);
    state.job.attempt = next_attempt;
    if (next_attempt != std::numeric_limits<uint64_t>::max()) {
        m_next_attempt = next_attempt + 1U;
    }
    state.job.cancellation_source = std::make_shared<std::stop_source>();
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
            if (result.regional_artifact != nullptr) {
                m_regional_result_bytes -= regionalArtifactStorageBytes(result.regional_artifact);
            }
            return true;
        });
        m_jobs.erase(iterator);
    } else {
        iterator->second.cancelled = true;
        if (iterator->second.job.cancellation_source != nullptr) {
            iterator->second.job.cancellation_source->request_stop();
        }
    }
    return true;
}

void WorldGenerationScheduler::cancelQueued() noexcept
{
    for (GenerationJobId const id : m_queue) {
        auto const state = m_jobs.find(id);
        if (state != m_jobs.end()) {
            if (state->second.job.cancellation_source != nullptr) {
                state->second.job.cancellation_source->request_stop();
            }
            m_jobs.erase(state);
        }
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
                if (state->second.job.cancellation_source != nullptr) {
                    state->second.job.cancellation_source->request_stop();
                }
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
            if (state.job.cancellation_source != nullptr) {
                state.job.cancellation_source->request_stop();
            }
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
        if (result.regional_artifact != nullptr) {
            m_regional_result_bytes -= regionalArtifactStorageBytes(result.regional_artifact);
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

uint64_t WorldGenerationScheduler::regionalResultBytes() const noexcept
{
    return m_regional_result_bytes;
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

uint64_t WorldGenerationCoordinator::RegionalArtifactKeyHash::operator()(
    RegionalArtifactKey const& key
) const noexcept
{
    uint64_t hash = RefinementOutputHash{}(key.region);
    hash ^= key.terrain_identity.terrain_version;
    hash *= 1'099'511'628'211ULL;
    hash ^= key.terrain_identity.revision;
    hash *= 1'099'511'628'211ULL;
    hash ^= key.terrain_identity.seed;
    hash *= 1'099'511'628'211ULL;
    for (char const byte : key.terrain_identity.configuration_identity) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1'099'511'628'211ULL;
    }
    hash ^= key.world_epoch;
    hash *= 1'099'511'628'211ULL;
    return hash;
}

WorldGenerationCoordinator::WorldGenerationCoordinator(
    uint64_t const max_pending_jobs, // NOLINT(bugprone-easily-swappable-parameters) -- capacity/revision/seed order
    uint64_t const revision,
    uint64_t const seed
)
    : m_scheduler{max_pending_jobs}
    , m_max_pending_jobs{max_pending_jobs}
    , m_revision{revision}
    , m_seed{seed}
{
    m_pipelines.reserve(static_cast<decltype(m_pipelines)::size_type>(max_pending_jobs));
    m_region_flights.reserve(static_cast<decltype(m_region_flights)::size_type>(max_pending_jobs));
    m_regional_artifacts.reserve(static_cast<decltype(m_regional_artifacts)::size_type>(max_pending_jobs));
    m_refinement_outputs.reserve(static_cast<decltype(m_refinement_outputs)::size_type>(
        std::min<uint64_t>(max_pending_jobs, MAX_GENERATION_OUTPUT_BUDGET_BYTES / MAX_GENERATION_STAGE_OUTPUT_BYTES)
    ));
}

void WorldGenerationCoordinator::setTerrainContext(
    std::shared_ptr<TerrainWorld const> terrain_world,
    uint64_t const world_epoch
)
{
    if (terrain_world == nullptr || world_epoch == 0U
        || terrain_world->identity().revision != m_revision
        || terrain_world->identity().seed != m_seed) {
        throw std::invalid_argument{"typed terrain context must match the active nonzero world epoch"};
    }
    if (m_terrain_world == terrain_world && m_world_epoch == world_epoch) {
        return;
    }
    for (auto& [identity, pipeline] : m_pipelines) {
        static_cast<void>(identity);
        if (pipeline.active_job_id != 0U) {
            static_cast<void>(m_scheduler.cancel(pipeline.active_job_id));
        }
        pipeline.state = GenerationState::Cancelled;
        pipeline.active_job_id = 0U;
        pipeline.result_ready = false;
        pipeline.result_output.reset();
        pipeline.result_region.reset();
        pipeline.parent_region.reset();
        pipeline.final_refinement_output.reset();
    }
    m_region_flights.clear();
    m_regional_artifacts.clear();
    m_regional_artifact_bytes = 0U;
    m_regional_artifact_reserved_bytes = 0U;
    m_refinement_outputs.clear();
    m_refinement_output_bytes = 0U;
    m_terrain_world = std::move(terrain_world);
    m_terrain_identity = m_terrain_world->identity();
    m_world_epoch = world_epoch;
}

std::optional<GenerationJob> WorldGenerationCoordinator::bindTerrainLeaf(GenerationJob job)
{
    if (m_terrain_world == nullptr || m_world_epoch == 0U
        || (job.stage != GenerationStage::HeightTile && job.stage != GenerationStage::Materialize)) {
        return std::nullopt;
    }
    auto const region = generationRegionForChunk(
        job.coordinate,
        REGISTERED_WORLD_GENERATION_EXTENTS.back(),
        GenerationStage::Refinement,
        m_revision,
        m_seed
    );
    if (!region.has_value()) {
        return std::nullopt;
    }
    RegionalArtifactKey const key{
        .region = *region,
        .terrain_identity = m_terrain_identity,
        .world_epoch = m_world_epoch,
    };
    auto const artifact = m_regional_artifacts.find(key);
    if (artifact == m_regional_artifacts.end()) {
        return std::nullopt;
    }
    artifact->second.last_access = ++m_regional_artifact_access_clock;
    job.terrain_identity = m_terrain_identity;
    job.world_epoch = m_world_epoch;
    job.seed = m_seed;
    if (job.attempt == 0U) {
        job.attempt = m_next_attempt++;
    }
    job.inherited_region = artifact->second.artifact;
    if (job.cancellation_source == nullptr) {
        job.cancellation_source = std::make_shared<std::stop_source>();
    }
    return job;
}

std::optional<WorldGenerationCoordinator::RegionalArtifactKey>
WorldGenerationCoordinator::parentArtifactKey(GenerationRegionKey const& region) const
{
    auto const extent = std::ranges::find(REGISTERED_WORLD_GENERATION_EXTENTS, region.extent);
    if (extent == REGISTERED_WORLD_GENERATION_EXTENTS.end()
        || extent == REGISTERED_WORLD_GENERATION_EXTENTS.begin()) {
        return std::nullopt;
    }
    uint32_t const parent_extent = *std::prev(extent);
    return RegionalArtifactKey{
        .region = GenerationRegionKey{
            .revision = region.revision,
            .seed = region.seed,
            .origin_x = static_cast<int32_t>(
                (static_cast<uint32_t>(region.origin_x) / parent_extent) * parent_extent
            ),
            .origin_y = static_cast<int32_t>(
                (static_cast<uint32_t>(region.origin_y) / parent_extent) * parent_extent
            ),
            .extent = parent_extent,
            .stage = parent_extent == REGISTERED_WORLD_GENERATION_EXTENTS.front()
                ? GenerationStage::Pregen
                : GenerationStage::Refinement,
        },
        .terrain_identity = m_terrain_identity,
        .world_epoch = m_world_epoch,
    };
}

bool WorldGenerationCoordinator::retainRegionalArtifact(
    RegionalArtifactKey const& key,
    std::shared_ptr<RegionalArtifact const> artifact
)
{
    uint64_t const storage_bytes = regionalArtifactStorageBytes(artifact);
    if (artifact == nullptr || storage_bytes == 0U
        || storage_bytes > MAX_GENERATION_REGIONAL_RETENTION_BUDGET_BYTES) {
        return false;
    }
    auto const existing = m_regional_artifacts.find(key);
    if (existing != m_regional_artifacts.end()) {
        if (existing->second.artifact != artifact) {
            return false;
        }
        existing->second.last_access = ++m_regional_artifact_access_clock;
        return true;
    }
    while (storage_bytes > MAX_GENERATION_REGIONAL_RETENTION_BUDGET_BYTES
        - std::min(
            m_regional_artifact_bytes + m_regional_artifact_reserved_bytes,
            MAX_GENERATION_REGIONAL_RETENTION_BUDGET_BYTES
        )) {
        auto oldest = m_regional_artifacts.end();
        for (auto candidate = m_regional_artifacts.begin(); candidate != m_regional_artifacts.end(); ++candidate) {
            if (candidate->second.artifact.use_count() != 1) {
                continue;
            }
            if (oldest == m_regional_artifacts.end()
                || candidate->second.last_access < oldest->second.last_access) {
                oldest = candidate;
            }
        }
        if (oldest == m_regional_artifacts.end()) {
            return false;
        }
        m_regional_artifact_bytes -= oldest->second.storage_bytes;
        m_regional_artifacts.erase(oldest);
    }
    m_regional_artifact_bytes += storage_bytes;
    m_regional_artifacts.emplace(key, RegionalArtifactEntry{
        .artifact = std::move(artifact),
        .storage_bytes = storage_bytes,
        .last_access = ++m_regional_artifact_access_clock,
    });
    return true;
}

bool WorldGenerationCoordinator::reserveRegionalArtifactStorage(uint64_t const bytes)
{
    if (bytes == 0U || bytes > MAX_GENERATION_REGIONAL_RETENTION_BUDGET_BYTES) {
        return false;
    }
    while (bytes > MAX_GENERATION_REGIONAL_RETENTION_BUDGET_BYTES
        - std::min(
            m_regional_artifact_bytes + m_regional_artifact_reserved_bytes,
            MAX_GENERATION_REGIONAL_RETENTION_BUDGET_BYTES
        )) {
        auto oldest = m_regional_artifacts.end();
        for (auto candidate = m_regional_artifacts.begin(); candidate != m_regional_artifacts.end(); ++candidate) {
            if (candidate->second.artifact.use_count() != 1) {
                continue;
            }
            if (oldest == m_regional_artifacts.end()
                || candidate->second.last_access < oldest->second.last_access) {
                oldest = candidate;
            }
        }
        if (oldest == m_regional_artifacts.end()) {
            return false;
        }
        m_regional_artifact_bytes -= oldest->second.storage_bytes;
        m_regional_artifacts.erase(oldest);
    }
    m_regional_artifact_reserved_bytes += bytes;
    return true;
}

WorldGenerationCoordinator::RegionFlight* WorldGenerationCoordinator::flightFor(
    GenerationJobId const id
) noexcept
{
    for (auto& [key, flight] : m_region_flights) {
        static_cast<void>(key);
        if (flight.job_id == id) {
            return &flight;
        }
    }
    return nullptr;
}

WorldGenerationCoordinator::RegionFlight const* WorldGenerationCoordinator::flightFor(
    GenerationJobId const id
) const noexcept
{
    for (auto const& [key, flight] : m_region_flights) {
        static_cast<void>(key);
        if (flight.job_id == id) {
            return &flight;
        }
    }
    return nullptr;
}

void WorldGenerationCoordinator::discardFlight(
    GenerationJobId const id,
    GenerationState const state
) noexcept
{
    auto flight = std::ranges::find_if(m_region_flights, [id](auto const& item) {
        return item.second.job_id == id;
    });
    if (flight == m_region_flights.end()) {
        return;
    }
    m_regional_artifact_reserved_bytes -= flight->second.reserved_bytes;
    for (GenerationIdentity const& subscriber : flight->second.subscribers) {
        auto const pipeline = m_pipelines.find(subscriber);
        if (pipeline == m_pipelines.end() || pipeline->second.active_job_id != id) {
            continue;
        }
        pipeline->second.state = state;
        pipeline->second.active_job_id = 0U;
        pipeline->second.result_ready = false;
        pipeline->second.result_output.reset();
        pipeline->second.result_region.reset();
        pipeline->second.parent_region.reset();
        pipeline->second.final_refinement_output.reset();
    }
    m_region_flights.erase(flight);
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
        .refinement_extents = {},
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
    if (m_terrain_world != nullptr) {
        if (m_world_epoch == 0U || !plan.pre_generate
            || !std::ranges::equal(plan.refinement_extents, REQUIRED_TYPED_REFINEMENT_EXTENTS)) {
            return GenerationAdmission::InvalidPlan;
        }
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
                || existing->second.state == GenerationState::Cancelled
                || existing->second.state == GenerationState::Failed);
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
        stages.push_back(PlanStage{
            .stage = stage,
            .region = std::nullopt,
        });
    }

    auto const [pipeline, inserted] = m_pipelines.emplace(identity, Pipeline{
        .stages = std::move(stages),
        .priority_scores = priority_scores,
        .state = GenerationState::Queued,
        .result_output = nullptr,
        .final_refinement_output = nullptr,
        .result_region = nullptr,
        .parent_region = nullptr,
    });
    if (!inserted) {
        return GenerationAdmission::Duplicate;
    }
    GenerationAdmission const admission = submitNextStage(identity, pipeline->second);
    if (admission != GenerationAdmission::Accepted && admission != GenerationAdmission::QueueFull
        && admission != GenerationAdmission::CapacityExceeded) {
        m_pipelines.erase(pipeline);
        return admission;
    }
    return GenerationAdmission::Accepted;
}

bool WorldGenerationCoordinator::updatePriority(
    ChunkCoordinate const coordinate,
    uint64_t const revision,
    uint64_t const seed,
    GenerationPriorityScores const priority_scores
)
{
    auto const pipeline = m_pipelines.find(GenerationIdentity{
        .coordinate = coordinate,
        .revision = revision,
        .seed = seed,
    });
    if (pipeline == m_pipelines.end() || pipeline->second.state != GenerationState::Queued) {
        return false;
    }
    pipeline->second.priority_scores = priority_scores;
    if (RegionFlight* const flight = flightFor(pipeline->second.active_job_id); flight != nullptr) {
        bool first = true;
        GenerationPriorityScores aggregate{};
        for (GenerationIdentity const& subscriber : flight->subscribers) {
            auto const current = m_pipelines.find(subscriber);
            if (current == m_pipelines.end()) {
                continue;
            }
            if (first) {
                aggregate = current->second.priority_scores;
                first = false;
            } else {
                aggregate.distance = std::min(aggregate.distance, current->second.priority_scores.distance);
                aggregate.view = std::min(aggregate.view, current->second.priority_scores.view);
                aggregate.movement = std::min(aggregate.movement, current->second.priority_scores.movement);
            }
        }
        if (!first) {
            flight->priority_scores = aggregate;
            flight->priority_initialized = true;
            if (!m_scheduler.updatePriority(flight->job_id, aggregate)) {
                return false;
            }
        }
    } else if (pipeline->second.active_job_id != 0U
        && !m_scheduler.updatePriority(pipeline->second.active_job_id, priority_scores)) {
        return false;
    }
    static_cast<void>(m_scheduler.reorderQueued(m_priority_order));
    return true;
}

std::optional<GenerationJob> WorldGenerationCoordinator::takeNext()
{
    std::vector<std::pair<GenerationIdentity const*, Pipeline*>> waiting;
    waiting.reserve(m_pipelines.size());
    for (auto& [identity, pipeline] : m_pipelines) {
        if (pipeline.state == GenerationState::Queued && pipeline.active_job_id == 0U) {
            waiting.emplace_back(&identity, &pipeline);
        }
    }
    std::ranges::sort(waiting, [this](auto const& first, auto const& second) {
        for (auto const dimension : m_priority_order) {
            uint32_t const first_score = priorityScore(first.second->priority_scores, dimension);
            uint32_t const second_score = priorityScore(second.second->priority_scores, dimension);
            if (first_score != second_score) {
                return first_score < second_score;
            }
        }
        auto const& first_identity = *first.first;
        auto const& second_identity = *second.first;
        return std::tuple{
            first_identity.revision,
            first_identity.seed,
            first_identity.coordinate.z,
            first_identity.coordinate.y,
            first_identity.coordinate.x,
        } < std::tuple{
            second_identity.revision,
            second_identity.seed,
            second_identity.coordinate.z,
            second_identity.coordinate.y,
            second_identity.coordinate.x,
        };
    });
    for (auto const& [identity, pipeline] : waiting) {
        static_cast<void>(submitNextStage(*identity, *pipeline));
    }
    static_cast<void>(m_scheduler.reorderQueued(m_priority_order));
    std::optional<GenerationJob> job = m_scheduler.takeNext();
    if (job.has_value()) {
        if (RegionFlight* const flight = flightFor(job->id); flight != nullptr) {
            for (GenerationIdentity const& subscriber : flight->subscribers) {
                auto const pipeline = m_pipelines.find(subscriber);
                if (pipeline != m_pipelines.end()) {
                    pipeline->second.state = GenerationState::Running;
                    pipeline->second.attempt = job->attempt;
                }
            }
        } else if (Pipeline* const pipeline = pipelineFor(job->id); pipeline != nullptr) {
            pipeline->state = GenerationState::Running;
            pipeline->attempt = job->attempt;
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
    if (cancelled && flightFor(id) != nullptr) {
        discardFlight(id, GenerationState::Cancelled);
        return true;
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

bool WorldGenerationCoordinator::complete(
    GenerationJob const& completed_job,
    bool const succeeded,
    bool const cancelled
)
{
    if (!m_scheduler.complete(completed_job, succeeded, cancelled)) {
        return false;
    }
    if (cancelled && flightFor(completed_job.id) != nullptr) {
        discardFlight(completed_job.id, GenerationState::Cancelled);
    } else if (cancelled) {
        if (Pipeline* const pipeline = pipelineFor(completed_job.id); pipeline != nullptr) {
            pipeline->state = GenerationState::Cancelled;
            pipeline->active_job_id = 0U;
            pipeline->result_ready = false;
            pipeline->result_output.reset();
            pipeline->final_refinement_output.reset();
            pipeline->result_region.reset();
            pipeline->parent_region.reset();
        }
    }
    return true;
}

bool WorldGenerationCoordinator::completeRegional(
    GenerationJob const& completed_job,
    bool const succeeded,
    bool const cancelled,
    std::shared_ptr<RegionalArtifact const> output
)
{
    if (!m_scheduler.completeRegional(completed_job, succeeded, cancelled, std::move(output))) {
        return false;
    }
    if (cancelled && flightFor(completed_job.id) != nullptr) {
        discardFlight(completed_job.id, GenerationState::Cancelled);
    }
    return true;
}

std::optional<GenerationResult> WorldGenerationCoordinator::takeResult() noexcept
{
    std::optional<GenerationResult> result = m_scheduler.takeResult();
    if (!result.has_value()) {
        return std::nullopt;
    }
    if (RegionFlight* const flight = flightFor(result->job.id); flight != nullptr) {
        flight->result = result->regional_artifact;
        flight->result_ready = true;
        for (GenerationIdentity const& subscriber : flight->subscribers) {
            auto const pipeline = m_pipelines.find(subscriber);
            if (pipeline == m_pipelines.end() || pipeline->second.active_job_id != result->job.id) {
                continue;
            }
            pipeline->second.result_ready = true;
            pipeline->second.result_region = result->regional_artifact;
            pipeline->second.state = result->succeeded ? GenerationState::Running : GenerationState::Failed;
        }
        if (!result->succeeded && result->job.retries >= WorldGenerationScheduler::MAX_RETRIES) {
            auto const entry = std::ranges::find_if(m_region_flights, [id = result->job.id](auto const& item) {
                return item.second.job_id == id;
            });
            if (entry != m_region_flights.end()) {
                m_regional_artifact_reserved_bytes -= entry->second.reserved_bytes;
                for (GenerationIdentity const& subscriber : entry->second.subscribers) {
                    auto const pipeline = m_pipelines.find(subscriber);
                    if (pipeline != m_pipelines.end() && pipeline->second.active_job_id == result->job.id) {
                        pipeline->second.active_job_id = 0U;
                        pipeline->second.result_ready = false;
                        pipeline->second.result_region.reset();
                        pipeline->second.parent_region.reset();
                        pipeline->second.final_refinement_output.reset();
                    }
                }
                m_region_flights.erase(entry);
            }
        }
    } else if (Pipeline* const pipeline = pipelineFor(result->job.id); pipeline != nullptr) {
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
            if (!result->succeeded && result->job.retries >= WorldGenerationScheduler::MAX_RETRIES) {
                pipeline->active_job_id = 0U;
                pipeline->result_ready = false;
                pipeline->result_output.reset();
                pipeline->final_refinement_output.reset();
                pipeline->result_region.reset();
                pipeline->parent_region.reset();
            }
        }
    }
    return result;
}

bool WorldGenerationCoordinator::acceptResult(GenerationJobId const id)
{
    if (RegionFlight* const flight = flightFor(id); flight != nullptr) {
        if (!flight->result_ready || flight->result == nullptr) {
            return false;
        }
        auto key = std::ranges::find_if(m_region_flights, [id](auto const& item) {
            return item.second.job_id == id;
        });
        if (key == m_region_flights.end()) {
            return false;
        }
        std::shared_ptr<RegionalArtifact const> const artifact = flight->result;
        m_regional_artifact_reserved_bytes -= flight->reserved_bytes;
        if (artifact->identity() != m_terrain_identity
            || !retainRegionalArtifact(key->first, artifact)) {
            for (GenerationIdentity const& subscriber : flight->subscribers) {
                auto const pipeline = m_pipelines.find(subscriber);
                if (pipeline != m_pipelines.end()) {
                    pipeline->second.state = GenerationState::Failed;
                    pipeline->second.active_job_id = 0U;
                    pipeline->second.result_ready = false;
                    pipeline->second.result_region.reset();
                    pipeline->second.parent_region.reset();
                    pipeline->second.final_refinement_output.reset();
                }
            }
            m_region_flights.erase(key);
            return false;
        }

        std::vector<GenerationIdentity> subscribers = std::move(flight->subscribers);
        m_region_flights.erase(key);
        bool accepted = false;
        for (GenerationIdentity const& subscriber : subscribers) {
            auto const pipeline = m_pipelines.find(subscriber);
            if (pipeline == m_pipelines.end() || pipeline->second.active_job_id != id
                || !pipeline->second.result_ready) {
                continue;
            }
            Pipeline& current = pipeline->second;
            if (subscriber.revision != m_revision || subscriber.seed != m_seed
                || m_world_epoch == 0U || artifact->identity() != m_terrain_identity) {
                current.state = GenerationState::Cancelled;
                current.active_job_id = 0U;
                current.result_ready = false;
                current.result_region.reset();
                current.parent_region.reset();
                continue;
            }
            current.parent_region = artifact;
            current.result_ready = false;
            current.result_region.reset();
            ++current.stage_index;
            current.active_job_id = 0U;
            GenerationAdmission const admission = submitNextStage(subscriber, current);
            if (admission == GenerationAdmission::QueueFull
                || admission == GenerationAdmission::CapacityExceeded) {
                current.state = GenerationState::Queued;
            } else if (admission == GenerationAdmission::InvalidPlan
                || admission == GenerationAdmission::Duplicate) {
                current.state = GenerationState::Failed;
                current.parent_region.reset();
            }
            accepted = true;
        }
        return accepted;
    }

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
    if (completed_stage.region.has_value() && pipeline->result_region != nullptr) {
        pipeline->parent_region = pipeline->result_region;
    }
    pipeline->result_ready = false;
    pipeline->result_output.reset();
    pipeline->result_region.reset();
    ++pipeline->stage_index;
    pipeline->active_job_id = 0U;
    GenerationAdmission const admission = submitNextStage(identity, *pipeline);
    if (admission == GenerationAdmission::InvalidPlan || admission == GenerationAdmission::Duplicate) {
        pipeline->state = GenerationState::Failed;
        pipeline->parent_region.reset();
        return false;
    }
    return true;
}

bool WorldGenerationCoordinator::retry(GenerationJobId const id)
{
    if (RegionFlight* const flight = flightFor(id); flight != nullptr) {
        if (!flight->result_ready || !m_scheduler.retry(id)) {
            return false;
        }
        flight->result.reset();
        flight->result_ready = false;
        for (GenerationIdentity const& subscriber : flight->subscribers) {
            auto const pipeline = m_pipelines.find(subscriber);
            if (pipeline != m_pipelines.end() && pipeline->second.active_job_id == id) {
                pipeline->second.state = GenerationState::Queued;
                pipeline->second.result_ready = false;
                pipeline->second.result_region.reset();
                pipeline->second.attempt = 0U;
            }
        }
        return true;
    }
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
)
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
    GenerationIdentity const identity{
        .coordinate = coordinate,
        .revision = revision,
        .seed = seed,
    };
    auto const pipeline = m_pipelines.find(identity);
    if (pipeline == m_pipelines.end() || pipeline->second.state == GenerationState::Cancelled
        || (pipeline->second.active_job_id == 0U && pipeline->second.state != GenerationState::Queued)) {
        return false;
    }
    GenerationJobId const job_id = pipeline->second.active_job_id;
    if (RegionFlight* const flight = flightFor(job_id); job_id != 0U && flight != nullptr) {
        std::erase(flight->subscribers, identity);
        if (flight->subscribers.empty()) {
            static_cast<void>(m_scheduler.cancel(job_id));
            auto flight_entry = std::ranges::find_if(m_region_flights, [job_id](auto const& item) {
                return item.second.job_id == job_id;
            });
            if (flight_entry != m_region_flights.end()) {
                m_regional_artifact_reserved_bytes -= flight_entry->second.reserved_bytes;
                m_region_flights.erase(flight_entry);
            }
        }
    } else if (job_id != 0U) {
        static_cast<void>(m_scheduler.cancel(job_id));
    }
    pipeline->second.state = GenerationState::Cancelled;
    pipeline->second.active_job_id = 0U;
    pipeline->second.result_ready = false;
    pipeline->second.result_output.reset();
    pipeline->second.final_refinement_output.reset();
    pipeline->second.result_region.reset();
    pipeline->second.parent_region.reset();
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
        pipeline.result_region.reset();
        pipeline.parent_region.reset();
        pipeline.final_refinement_output.reset();
    }
    m_region_flights.clear();
    m_regional_artifacts.clear();
    m_regional_artifact_bytes = 0U;
    m_regional_artifact_reserved_bytes = 0U;
    m_refinement_outputs.clear();
    m_refinement_output_bytes = 0U;
    m_revision = revision;
    m_seed = seed;
    m_terrain_world.reset();
    m_terrain_identity = {};
    m_world_epoch = 0U;
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

uint64_t WorldGenerationCoordinator::regionalArtifactBytes() const noexcept
{
    return m_regional_artifact_bytes + m_regional_artifact_reserved_bytes;
}

GenerationAdmission WorldGenerationCoordinator::submitNextStage(
    GenerationIdentity const& identity,
    Pipeline& pipeline
)
{
    if (m_terrain_world != nullptr && pipeline.active_job_id == 0U && pipeline.parent_region == nullptr) {
        uint64_t const active_pipelines = static_cast<uint64_t>(std::ranges::count_if(
            m_pipelines,
            [](auto const& entry) {
                return entry.second.active_job_id != 0U || entry.second.parent_region != nullptr;
            }
        ));
        if (active_pipelines >= MAX_ACTIVE_TERRAIN_PIPELINES) {
            return GenerationAdmission::CapacityExceeded;
        }
    }
    while (pipeline.stage_index < pipeline.stages.size()) {
        PlanStage const& stage = pipeline.stages[
            static_cast<std::vector<PlanStage>::size_type>(pipeline.stage_index)
        ];
        if (m_terrain_world != nullptr) {
            if (m_world_epoch == 0U || m_terrain_world->identity() != m_terrain_identity
                || identity.revision != m_terrain_identity.revision
                || identity.seed != m_terrain_identity.seed) {
                return GenerationAdmission::InvalidPlan;
            }
            if (stage.region.has_value()) {
                RegionalArtifactKey const key{
                    .region = *stage.region,
                    .terrain_identity = m_terrain_identity,
                    .world_epoch = m_world_epoch,
                };
                auto const retained = m_regional_artifacts.find(key);
                if (retained != m_regional_artifacts.end()) {
                    retained->second.last_access = ++m_regional_artifact_access_clock;
                    pipeline.parent_region = retained->second.artifact;
                    ++pipeline.stage_index;
                    continue;
                }
                auto const flight_entry = m_region_flights.find(key);
                if (flight_entry != m_region_flights.end()) {
                    RegionFlight& flight = flight_entry->second;
                    if (std::ranges::find(flight.subscribers, identity) == flight.subscribers.end()) {
                        flight.subscribers.push_back(identity);
                    }
                    if (!flight.priority_initialized) {
                        flight.priority_scores = pipeline.priority_scores;
                        flight.priority_initialized = true;
                    } else {
                        flight.priority_scores.distance = std::min(
                            flight.priority_scores.distance,
                            pipeline.priority_scores.distance
                        );
                        flight.priority_scores.view = std::min(
                            flight.priority_scores.view,
                            pipeline.priority_scores.view
                        );
                        flight.priority_scores.movement = std::min(
                            flight.priority_scores.movement,
                            pipeline.priority_scores.movement
                        );
                    }
                    static_cast<void>(m_scheduler.updatePriority(flight.job_id, flight.priority_scores));
                    pipeline.active_job_id = flight.job_id;
                    pipeline.state = GenerationState::Queued;
                    static_cast<void>(m_scheduler.reorderQueued(m_priority_order));
                    return GenerationAdmission::Accepted;
                }

                std::shared_ptr<RegionalArtifact const> inherited_region;
                if (stage.stage == GenerationStage::Pregen) {
                    if (stage.region->extent != REGISTERED_WORLD_GENERATION_EXTENTS.front()
                        || pipeline.parent_region != nullptr) {
                        return GenerationAdmission::InvalidPlan;
                    }
                } else {
                    auto const parent_key = parentArtifactKey(*stage.region);
                    if (!parent_key.has_value() || pipeline.parent_region == nullptr) {
                        return GenerationAdmission::InvalidPlan;
                    }
                    TerrainRegionKey const expected_parent{
                        .origin_x = static_cast<uint32_t>(parent_key->region.origin_x),
                        .origin_y = static_cast<uint32_t>(parent_key->region.origin_y),
                        .extent = parent_key->region.extent,
                    };
                    if (pipeline.parent_region->identity() != m_terrain_identity
                        || pipeline.parent_region->key() != expected_parent) {
                        return GenerationAdmission::InvalidPlan;
                    }
                    inherited_region = pipeline.parent_region;
                }
                uint64_t const reserved_bytes = maximumRegionalArtifactStorageBytes();
                if (!reserveRegionalArtifactStorage(reserved_bytes)) {
                    return GenerationAdmission::CapacityExceeded;
                }
                WorldGenerationScheduler::AdmissionResult const submitted = m_scheduler.submitJob(
                    identity.coordinate,
                    identity.revision,
                    identity.seed,
                    stage.stage,
                    pipeline.priority_scores,
                    stage.region,
                    nullptr,
                    m_terrain_identity,
                    WorldGenerationScheduler::SubmissionAttempt{
                        .world_epoch = m_world_epoch,
                        .attempt = m_next_attempt++,
                    },
                    inherited_region
                );
                if (submitted.admission == GenerationAdmission::QueueFull) {
                    m_regional_artifact_reserved_bytes -= reserved_bytes;
                    pipeline.state = GenerationState::Queued;
                    pipeline.active_job_id = 0U;
                    return GenerationAdmission::QueueFull;
                }
                if (submitted.admission != GenerationAdmission::Accepted || !submitted.job.has_value()) {
                    m_regional_artifact_reserved_bytes -= reserved_bytes;
                    return submitted.admission;
                }
                pipeline.active_job_id = submitted.job->id;
                pipeline.state = GenerationState::Queued;
                m_region_flights.emplace(key, RegionFlight{
                    .job_id = submitted.job->id,
                    .subscribers = {identity},
                    .priority_scores = pipeline.priority_scores,
                    .priority_initialized = true,
                    .reserved_bytes = reserved_bytes,
                    .result = nullptr,
                    .result_ready = false,
                });
                static_cast<void>(m_scheduler.reorderQueued(m_priority_order));
                return GenerationAdmission::Accepted;
            }

            auto const leaf_region = generationRegionForChunk(
                identity.coordinate,
                REGISTERED_WORLD_GENERATION_EXTENTS.back(),
                GenerationStage::Refinement,
                identity.revision,
                identity.seed
            );
            if (!leaf_region.has_value()) {
                return GenerationAdmission::InvalidPlan;
            }
            if (pipeline.parent_region == nullptr
                || pipeline.parent_region->identity() != m_terrain_identity
                || pipeline.parent_region->key()
                    != TerrainRegionKey{
                        .origin_x = static_cast<uint32_t>(leaf_region->origin_x),
                        .origin_y = static_cast<uint32_t>(leaf_region->origin_y),
                        .extent = leaf_region->extent,
                    }) {
                RegionalArtifactKey const leaf_key{
                    .region = *leaf_region,
                    .terrain_identity = m_terrain_identity,
                    .world_epoch = m_world_epoch,
                };
                auto const leaf = m_regional_artifacts.find(leaf_key);
                if (leaf == m_regional_artifacts.end()) {
                    return GenerationAdmission::InvalidPlan;
                }
                leaf->second.last_access = ++m_regional_artifact_access_clock;
                pipeline.parent_region = leaf->second.artifact;
            }
            WorldGenerationScheduler::AdmissionResult const submitted = m_scheduler.submitJob(
                identity.coordinate,
                identity.revision,
                identity.seed,
                stage.stage,
                pipeline.priority_scores,
                std::nullopt,
                nullptr,
                m_terrain_identity,
                WorldGenerationScheduler::SubmissionAttempt{
                    .world_epoch = m_world_epoch,
                    .attempt = m_next_attempt++,
                },
                pipeline.parent_region
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
    pipeline.parent_region.reset();
    pipeline.result_region.reset();
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
                || pipeline->second.state == GenerationState::Cancelled
                || pipeline->second.state == GenerationState::Failed);
        if (terminal) {
            pipeline = m_pipelines.erase(pipeline);
        } else {
            ++pipeline;
        }
    }
}

} // namespace shared
