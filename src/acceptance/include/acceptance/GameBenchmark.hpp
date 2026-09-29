#pragma once

#include <acceptance/EvidenceJson.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>

namespace acceptance {

struct GameBenchmarkOptions final {
    std::chrono::seconds cold_duration{ 2 };
    std::chrono::seconds warm_duration{ 3 };
    std::chrono::seconds uncapped_duration{ 1 };
    std::chrono::seconds deadline{ 60 };
    bool require_immediate_present_mode = false;
};

enum class GameBenchmarkPhase : uint8_t {
    Cold,
    Warm,
    Uncapped,
    Outside,
};

[[nodiscard]]
GameBenchmarkPhase gameBenchmarkPhaseAt(
    std::chrono::steady_clock::time_point completed_at,
    std::chrono::steady_clock::time_point first_loop_at,
    GameBenchmarkOptions const& options
) noexcept;

[[nodiscard]]
std::expected<RuntimeEvidence, std::string> runGameBenchmark(
    GameBenchmarkOptions const& options = { }
);

} // namespace acceptance
