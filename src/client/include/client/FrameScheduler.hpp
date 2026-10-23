#pragma once

#include <chrono>

namespace client {

class FrameScheduler final {
public:
    explicit FrameScheduler(
        std::chrono::steady_clock::time_point started_at,
        std::chrono::milliseconds simulation_period
    ) noexcept;

    [[nodiscard]]
    bool simulationDue(std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]]
    std::chrono::steady_clock::time_point lastSimulationBoundary() const noexcept
    {
        return m_last_simulation;
    }
    [[nodiscard]]
    std::chrono::steady_clock::duration idleDelay(
        std::chrono::steady_clock::time_point iteration_started_at,
        std::chrono::steady_clock::time_point iteration_completed_at,
        bool presentation_succeeded
    ) const noexcept;

private:
    std::chrono::steady_clock::time_point m_next_simulation;
    std::chrono::steady_clock::time_point m_last_simulation;
    std::chrono::milliseconds m_simulation_period;
};

} // namespace client
