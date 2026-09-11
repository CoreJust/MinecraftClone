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
    std::chrono::milliseconds idleDelay(std::chrono::steady_clock::time_point now) const noexcept;

private:
    std::chrono::steady_clock::time_point m_next_simulation;
    std::chrono::milliseconds m_simulation_period;
};

} // namespace client
