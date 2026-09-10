#pragma once

#include <shared/scenario/Scenario.hpp>

#include <acceptance/EvidenceJson.hpp>

#include <algorithm>
#include <chrono>
#include <expected>
#include <string>

namespace acceptance {

namespace detail {

[[nodiscard]]
constexpr std::chrono::milliseconds boundedNetworkPollTimeout(
    std::chrono::milliseconds const poll_interval,
    std::chrono::milliseconds const remaining
) noexcept
{
    return std::min(poll_interval, remaining);
}

} // namespace detail

struct ScenarioRunOptions final {
    std::chrono::seconds deadline{ 10 };
    std::chrono::milliseconds network_poll_interval{ 1 };
};

[[nodiscard]]
std::expected<RuntimeEvidence, std::string> runScenario(
    shared::ScenarioPlan const& plan,
    ScenarioRunOptions const& options = { }
);

} // namespace acceptance
