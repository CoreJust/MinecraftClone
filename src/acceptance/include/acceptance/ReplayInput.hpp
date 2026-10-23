#pragma once

#include <shared/world/World.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace client {
class PreviewResidency;
} // namespace client

namespace shared {
struct TerrainIdentity;
} // namespace shared

namespace acceptance {

inline constexpr std::chrono::seconds GAME_BENCHMARK_MOVEMENT_ROUTE_DURATION{ 30 };
inline constexpr double GAME_BENCHMARK_FLIGHT_CLEARANCE_TARGET_BLOCKS = 96.0;
inline constexpr double GAME_BENCHMARK_FLIGHT_CLEARANCE_HOLD_BAND_BLOCKS = 24.0;
inline constexpr double GAME_BENCHMARK_FLIGHT_CLEARANCE_LOOKAHEAD_BLOCKS = 64.0;
inline constexpr double GAME_BENCHMARK_UNKNOWN_TERRAIN_TARGET_BELOW_CEILING_BLOCKS = 32.0;

enum class GameBenchmarkWorkload : uint8_t {
    OrdinaryMovement,
    OrdinaryTurningMovement,
    Speed200Movement,
    WrappedBorder,
    PermissionCollisionChurn,
    DiagnosticStationary,
    Speed200FullRadiusRecovery,
};

struct GameBenchmarkMovementRouteSample final {
    int8_t strafe = 0;
    int8_t forward = 0;
    int8_t vertical = 0;
    double yaw_degrees = 0.0;
    double pitch_degrees = 0.0;

    bool operator==(GameBenchmarkMovementRouteSample const&) const noexcept = default;
};

struct GameBenchmarkStartActionEvidence final {
    std::string input_source = "synthetic-controlled-start-key";
    std::string production_path =
        "GameClientBenchmarkHooks.start_input_override->GameClient.acceptLoadingStartInput";
    bool synthetic_input_override_invoked = false;
    std::optional<uint64_t> eligible_prompt_rendered_offset_ns;
    std::optional<uint64_t> synthetic_input_requested_offset_ns;
    std::optional<uint64_t> key_accepted_offset_ns;
    std::optional<uint64_t> server_start_acknowledged_offset_ns;
    std::optional<uint64_t> input_ordinal;
    bool complete = false;
};

[[nodiscard]]
bool gameBenchmarkStartActionComplete(GameBenchmarkStartActionEvidence const& evidence) noexcept;

[[nodiscard]]
shared::Direction gameBenchmarkDirection(GameBenchmarkWorkload workload, uint64_t ordinal) noexcept;

[[nodiscard]]
shared::Direction gameBenchmarkDirection(GameBenchmarkMovementRouteSample const& route) noexcept;

[[nodiscard]]
GameBenchmarkMovementRouteSample gameBenchmarkMovementRouteAt(std::chrono::nanoseconds elapsed) noexcept;

[[nodiscard]]
GameBenchmarkMovementRouteSample gameBenchmarkRepeatingMovementRouteAt(std::chrono::nanoseconds elapsed) noexcept;

// Unknown terrain uses a bounded high-altitude target and never implies that a
// local surface height or readiness state was observed. Missing player Z holds.
[[nodiscard]]
int8_t gameBenchmarkFlightClearanceIntent(
    std::optional<double> player_z,
    std::optional<uint16_t> highest_solid_z
) noexcept;

[[nodiscard]]
GameBenchmarkMovementRouteSample gameBenchmarkApplyFlightClearance(
    GameBenchmarkMovementRouteSample route,
    std::optional<double> player_z,
    std::optional<uint16_t> highest_solid_z
) noexcept;

[[nodiscard]]
std::optional<uint16_t> gameBenchmarkDeliveredSurfaceHeight(
    client::PreviewResidency const& residency,
    shared::TerrainIdentity const* expected_identity,
    double world_x,
    double world_y
) noexcept;

[[nodiscard]]
uint64_t gameBenchmarkMovementRouteCycleAt(std::chrono::nanoseconds elapsed) noexcept;

[[nodiscard]]
std::string_view gameBenchmarkWorkloadName(GameBenchmarkWorkload workload) noexcept;

} // namespace acceptance
