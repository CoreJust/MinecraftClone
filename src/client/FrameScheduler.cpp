#include <client/FrameScheduler.hpp>

#include <algorithm>
#include <compare>
#include <ratio>

namespace client {

FrameScheduler::FrameScheduler(
    std::chrono::steady_clock::time_point const started_at,
    std::chrono::milliseconds const simulation_period
) noexcept
    : m_next_simulation(started_at)
    , m_last_simulation(started_at)
    , m_simulation_period(std::max(simulation_period, std::chrono::milliseconds{ 1 }))
{ }

bool FrameScheduler::simulationDue(std::chrono::steady_clock::time_point const now) noexcept
{
    if (now < m_next_simulation) {
        return false;
    }
    auto const periods = (now - m_next_simulation) / m_simulation_period + 1;
    m_next_simulation += m_simulation_period * periods;
    m_last_simulation = m_next_simulation - m_simulation_period;
    return true;
}

std::chrono::steady_clock::duration FrameScheduler::idleDelay(
    std::chrono::steady_clock::time_point const iteration_started_at,
    std::chrono::steady_clock::time_point const iteration_completed_at,
    bool const presentation_succeeded
) const noexcept
{
    std::chrono::steady_clock::duration const backoff = std::chrono::milliseconds{ 1 };
    std::chrono::steady_clock::duration const iteration_duration = iteration_completed_at - iteration_started_at;
    if (presentation_succeeded) {
        return std::chrono::steady_clock::duration::zero();
    }
    if (iteration_duration >= backoff) {
        return std::chrono::steady_clock::duration::zero();
    }
    if (iteration_completed_at >= m_next_simulation) {
        return std::chrono::steady_clock::duration::zero();
    }
    std::chrono::steady_clock::duration const remaining_backoff = backoff - iteration_duration;
    return std::min(remaining_backoff, m_next_simulation - iteration_completed_at);
}

} // namespace client
