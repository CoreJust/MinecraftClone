#pragma once

#include <acceptance/EvidenceJson.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace shared {

struct Direction;

} // namespace shared

namespace acceptance {

enum class GameBenchmarkWorkload : uint8_t {
    OrdinaryMovement,
    Speed200Movement,
    WrappedBorder,
    PermissionCollisionChurn,
    DiagnosticStationary,
};

struct GameBenchmarkOptions final {
    std::chrono::seconds cold_duration{ 2 };
    std::chrono::seconds warm_duration{ 3 };
    std::chrono::seconds uncapped_duration{ 1 };
    std::chrono::seconds deadline{ 60 };
    bool require_immediate_present_mode = false;
    GameBenchmarkWorkload workload = GameBenchmarkWorkload::OrdinaryMovement;
};

[[nodiscard]]
shared::Direction gameBenchmarkDirection(GameBenchmarkWorkload workload, uint64_t ordinal) noexcept;

[[nodiscard]]
std::string_view gameBenchmarkWorkloadName(GameBenchmarkWorkload workload) noexcept;

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
