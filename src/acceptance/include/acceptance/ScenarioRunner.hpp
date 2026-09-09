#pragma once

#include <acceptance/EvidenceJson.hpp>

#include <shared/scenario/Scenario.hpp>

#include <chrono>
#include <expected>
#include <string>

namespace acceptance {

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
