#pragma once

#include <shared/net/Message.hpp>
#include <shared/world/World.hpp>

#include <acceptance/EvidenceJson.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace acceptance {

inline constexpr int32_t GAME_BENCHMARK_TERRAIN_NEAR_RADIUS = 7;
inline constexpr uint32_t GAME_BENCHMARK_TERRAIN_NEAR_TILE_COUNT = 225U;
inline constexpr std::chrono::milliseconds GAME_BENCHMARK_TERRAIN_READINESS_DEADLINE{ 4'000 };
inline constexpr std::chrono::seconds GAME_BENCHMARK_FULL_RADIUS_DEFAULT_WARMUP_TIMEOUT{ 180 };
inline constexpr std::chrono::seconds GAME_BENCHMARK_FULL_RADIUS_MAX_WARMUP_TIMEOUT{ 1'800 };
inline constexpr std::chrono::seconds GAME_BENCHMARK_FULL_RADIUS_STATIONARY_SAMPLE{ 10 };
inline constexpr std::chrono::seconds GAME_BENCHMARK_FULL_RADIUS_MOVEMENT{ 6 };
inline constexpr std::chrono::seconds GAME_BENCHMARK_FULL_RADIUS_NON_WARMUP_BUDGET{ 56 };

enum class GameBenchmarkFullRadiusPhase : uint8_t {
    InitialWarmup,
    BaselineStationary,
    Movement,
    RecoveryWarmup,
    RecoveredStationary,
    Complete,
    Failed,
};

class GameBenchmarkFullRadiusSequence final {
public:
    explicit GameBenchmarkFullRadiusSequence(std::chrono::seconds warmup_timeout) noexcept;
    void start(std::chrono::steady_clock::time_point started_at) noexcept;
    void observe(
        std::chrono::steady_clock::time_point sampled_at,
        std::optional<bool> exact_current_coverage
    ) noexcept;
    [[nodiscard]] GameBenchmarkFullRadiusPhase phase() const noexcept { return m_phase; }
    [[nodiscard]] std::chrono::seconds warmupTimeout() const noexcept { return m_warmup_timeout; }
    [[nodiscard]] std::chrono::steady_clock::duration phaseElapsed(
        std::chrono::steady_clock::time_point sampled_at
    ) const noexcept;

private:
    void transition(GameBenchmarkFullRadiusPhase phase, std::chrono::steady_clock::time_point started_at) noexcept;

    std::chrono::seconds m_warmup_timeout;
    GameBenchmarkFullRadiusPhase m_phase = GameBenchmarkFullRadiusPhase::InitialWarmup;
    std::chrono::steady_clock::time_point m_phase_started_at{};
    bool m_started = false;
};

[[nodiscard]] std::expected<std::chrono::seconds, std::string> gameBenchmarkFullRadiusWarmupTimeout(
    int64_t seconds
);

[[nodiscard]]
std::vector<std::chrono::nanoseconds> gameBenchmarkSuccessfulPresentIntervals(
    std::span<GameBenchmarkFrameSample const> frames,
    std::string_view stationary_phase
);

[[nodiscard]]
bool gameBenchmarkRecoveryIsCloserToMovementEnd(
    shared::HeightTileKey movement_start,
    shared::HeightTileKey movement_end,
    shared::HeightTileKey recovery_center
) noexcept;

struct GameBenchmarkTerrainTileObservation final {
    shared::HeightTileKey key{};
    bool desired = false;
    bool resident = false;
    bool current_mesh_visible = false;
    bool pending = false;
    bool active = false;
};

struct GameBenchmarkTerrainObservation final {
    std::optional<shared::HeightTileKey> center;
    std::array<GameBenchmarkTerrainTileObservation, GAME_BENCHMARK_TERRAIN_NEAR_TILE_COUNT> near_tiles{};
    uint32_t desired_count = 0U;
    uint32_t resident_count = 0U;
    uint32_t nonresident_count = 0U;
    uint32_t current_mesh_visible_count = 0U;
    uint32_t pending_count = 0U;
    uint32_t active_count = 0U;
    uint32_t unclassified_missing_count = 0U;
    uint64_t accepted_near_admissions = 0U;
    uint64_t accepted_far_admissions = 0U;
    uint64_t far_admissions_while_near_pending = 0U;
};

class GameBenchmarkTerrainReadinessTracker final {
public:
    void observe(GameBenchmarkTerrainObservation const& observation,
        std::chrono::steady_clock::time_point sampled_at);
    [[nodiscard]] GameBenchmarkTerrainReadinessEvidence evidence() const noexcept;

private:
    struct Episode final {
        std::chrono::steady_clock::time_point started_at;
        shared::HeightTileKey start_center{};
        bool deadline_missed = false;
    };

    [[nodiscard]] static std::pair<int32_t, int32_t> coordinates(shared::HeightTileKey key) noexcept;

    std::map<std::pair<int32_t, int32_t>, Episode> m_open_episodes;
    GameBenchmarkTerrainReadinessEvidence m_evidence;
    std::optional<shared::HeightTileKey> m_last_center;
};

enum class GameBenchmarkWorkload : uint8_t {
    OrdinaryMovement,
    Speed200Movement,
    WrappedBorder,
    PermissionCollisionChurn,
    DiagnosticStationary,
    Speed200FullRadiusRecovery,
};

struct GameBenchmarkOptions final {
    std::chrono::seconds cold_duration{ 2 };
    std::chrono::seconds warm_duration{ 3 };
    std::chrono::seconds uncapped_duration{ 1 };
    std::chrono::seconds deadline{ 60 };
    bool require_immediate_present_mode = false;
    GameBenchmarkWorkload workload = GameBenchmarkWorkload::OrdinaryMovement;
    uint32_t render_distance = shared::HEIGHT_TILE_INTEREST_RADIUS;
    std::chrono::seconds full_radius_warmup_timeout = GAME_BENCHMARK_FULL_RADIUS_DEFAULT_WARMUP_TIMEOUT;
    bool diagnostic_full_radius_capture = false;
};

[[nodiscard]] std::chrono::seconds gameBenchmarkDeadline(GameBenchmarkOptions const& options) noexcept;

[[nodiscard]]
bool gameBenchmarkDiagnosticOverridesPresent(bool explicit_diagnostic_capture = false) noexcept;

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
