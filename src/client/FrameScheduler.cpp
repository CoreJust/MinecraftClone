#include <client/FrameScheduler.hpp>

#include <algorithm>

namespace client {

FrameScheduler::FrameScheduler(
    std::chrono::steady_clock::time_point const started_at,
    std::chrono::milliseconds const simulation_period
) noexcept
    : m_next_simulation(started_at)
    , m_simulation_period(std::max(simulation_period, std::chrono::milliseconds{ 1 }))
{ }

bool FrameScheduler::simulationDue(std::chrono::steady_clock::time_point const now) noexcept
{
    if (now < m_next_simulation) {
        return false;
    }
    m_next_simulation = now + m_simulation_period;
    return true;
}

std::chrono::milliseconds FrameScheduler::idleDelay(
    std::chrono::steady_clock::time_point const now
) const noexcept
{
    if (now >= m_next_simulation) {
        return std::chrono::milliseconds{ 1 };
    }
    return std::min(
        std::chrono::duration_cast<std::chrono::milliseconds>(m_next_simulation - now),
        std::chrono::milliseconds{ 1 }
    );
}

} // namespace client
