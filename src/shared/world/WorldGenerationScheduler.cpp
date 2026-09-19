#include <shared/world/WorldGenerationScheduler.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace shared {

WorldGenerationScheduler::WorldGenerationScheduler(uint64_t const max_pending_jobs)
    : m_max_pending_jobs(max_pending_jobs)
{
    if (m_max_pending_jobs == 0U) {
        throw std::invalid_argument{"world generation scheduler requires a positive queue bound"};
    }
    m_jobs.reserve(static_cast<decltype(m_jobs)::size_type>(max_pending_jobs));
}

GenerationAdmission WorldGenerationScheduler::submit(
    ChunkCoordinate const coordinate,
    uint64_t const revision,
    GenerationStage const stage
)
{
    for (auto const& [id, state] : m_jobs) {
        static_cast<void>(id);
        if (state.job.coordinate == coordinate
            && state.job.revision == revision
            && state.job.stage == stage
            && !state.stale
            && !state.cancelled) {
            return GenerationAdmission::Duplicate;
        }
    }
    if (m_queue.size() >= m_max_pending_jobs) {
        return GenerationAdmission::QueueFull;
    }
    GenerationJob const job{
        .id = m_next_id++,
        .coordinate = coordinate,
        .revision = revision,
        .stage = stage,
    };
    m_jobs.emplace(job.id, JobState{.job = job});
    m_queue.push_back(job.id);
    return GenerationAdmission::Accepted;
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

bool WorldGenerationScheduler::complete(
    GenerationJobId const id,
    bool const succeeded,
    bool const cancelled
) noexcept
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
    state.completed = true;
    if (m_results.size() < m_max_pending_jobs) {
        m_results.push_back(GenerationResult{
            .job = state.job,
            .succeeded = succeeded,
            .cancelled = false,
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
    if (state.running || !state.completed || state.stale || state.cancelled || state.job.retries >= MAX_RETRIES
        || m_queue.size() >= m_max_pending_jobs) {
        return false;
    }
    state.job.retries = static_cast<uint8_t>(state.job.retries + 1U);
    state.running = false;
    state.completed = false;
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
        m_jobs.erase(iterator);
    } else {
        iterator->second.cancelled = true;
    }
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
    if (m_results.empty()) {
        return std::nullopt;
    }
    GenerationResult result = std::move(m_results.front());
    m_results.pop_front();
    if (result.succeeded || result.job.retries >= MAX_RETRIES) {
        m_jobs.erase(result.job.id);
    }
    return result;
}

uint64_t WorldGenerationScheduler::pendingCount() const noexcept
{
    return static_cast<uint64_t>(m_queue.size());
}

uint64_t WorldGenerationScheduler::resultCount() const noexcept
{
    return static_cast<uint64_t>(m_results.size());
}

} // namespace shared
