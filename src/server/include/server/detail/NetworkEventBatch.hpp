#pragma once

#include <chrono>
#include <cstdint>

namespace server::detail {

inline constexpr uint32_t MAX_NETWORK_EVENTS_PER_BATCH = 256U;
inline constexpr std::chrono::milliseconds NETWORK_EVENT_DRAIN_BUDGET{1};

template<typename Poll, typename StopRequested>
[[nodiscard]]
uint64_t pollNetworkEventBatch(
    Poll&& poll,
    std::chrono::milliseconds const timeout,
    StopRequested&& stop_requested,
    std::chrono::milliseconds const drain_budget = NETWORK_EVENT_DRAIN_BUDGET,
    uint32_t const maximum_events = MAX_NETWORK_EVENTS_PER_BATCH
)
{
    if (stop_requested()) {
        return 0U;
    }

    uint64_t events = poll(timeout);
    auto const drain_deadline = std::chrono::steady_clock::now() + drain_budget;
    while (events > 0U
        && events < maximum_events
        && std::chrono::steady_clock::now() < drain_deadline
        && !stop_requested()) {
        uint32_t const drained = poll(std::chrono::milliseconds::zero());
        if (drained == 0U) {
            break;
        }
        events += drained;
    }
    return events;
}

} // namespace server::detail
