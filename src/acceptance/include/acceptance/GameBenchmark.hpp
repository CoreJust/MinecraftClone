#pragma once

#include <client/Camera.hpp>

#include <shared/corelang/ExecutionTypes.hpp>
#include <shared/net/Message.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/World.hpp>

#include <generation/Generation.hpp>

#include <acceptance/EvidenceJson.hpp>
#include <acceptance/ReplayInput.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace client {
struct TerrainLodScene;
} // namespace client

namespace acceptance {

inline constexpr int32_t GAME_BENCHMARK_TERRAIN_NEAR_RADIUS = 7;
inline constexpr uint32_t GAME_BENCHMARK_TERRAIN_NEAR_TILE_COUNT = 225U;
inline constexpr std::chrono::milliseconds GAME_BENCHMARK_TERRAIN_READINESS_DEADLINE{ 4'000 };
inline constexpr std::chrono::seconds GAME_BENCHMARK_FULL_RADIUS_DEFAULT_WARMUP_TIMEOUT{ 180 };
inline constexpr std::chrono::seconds GAME_BENCHMARK_FULL_RADIUS_MAX_WARMUP_TIMEOUT{ 1'800 };
inline constexpr std::chrono::seconds GAME_BENCHMARK_FULL_RADIUS_STATIONARY_SAMPLE{ 30 };
inline constexpr std::chrono::seconds GAME_BENCHMARK_FULL_RADIUS_MOVEMENT{ 30 };
inline constexpr std::chrono::seconds GAME_BENCHMARK_FULL_RADIUS_NON_WARMUP_BUDGET{ 120 };
inline constexpr std::chrono::seconds GAME_BENCHMARK_S1_MOVEMENT_WINDOW{ 30 };
inline constexpr uint32_t GAME_BENCHMARK_S1_EXPECTED_MOVEMENT_UPDATES = 300U;
inline constexpr std::string_view GAME_BENCHMARK_S2_NATIVE_CONFIGURATION_IDENTITY =
    "terrain-v10;source=f775213330028b165328885c6899ac0bf9c3c0c9a93ff6e0aca591554dec6e59"
    ";seed=42;resolution=256;platec=1.4.3-direct;foundation=v1;fine-climate=v1"
    ";fresh-climate=v7;scale=1;datum=512;depth=2048;native-v10-r1;water=v5-fine-bed-v1"
    ";river=c8f9b851f7891e555165648d473b39df1ecffc9b73ae65cf5b14ed9f5ba04703"
    ";fine=e52e2e82b3e9873e14e543e0a7583fb978a182f1555997bda6febcd9ff25061a";
inline constexpr std::string_view GAME_BENCHMARK_S1_QUALIFICATION_MANIFEST =
    "{\n"
    "  \"contract\": \"mc.s1-qualification-settings.v1\",\n"
    "  \"fov_degrees\": 70,\n"
    "  \"debug_hud_enabled\": true,\n"
    "  \"camera_perspective\": \"first-person\",\n"
    "  \"world_mode\": \"flight\",\n"
    "  \"terrain_model\": \"lod-canonical-r6-height-v2\",\n"
    "  \"terrain_identity\": {\"terrain_version\": 2, \"revision\": 1, \"seed\": 42, \"configuration_i"
    "dentity\": \"terrain-v2;model-revision=6;source-map-sha256=5de753a4068c71d742f5ce7e75e4e8b"
    "cabbaad5fddaf6c8404dc41ba00d45704;seed=42;resolution=256;epochs=12;erosion-steps=3;initi"
    "al-climate-iterations=24;climate-iterations=80;currents=1;wind-sign=1;coastal-elaboratio"
    "n=1;shelf-exp=exp64-to-f32-v1\"},\n"
    "  \"render_distance\": 256,\n"
    "  \"actual_radius_blocks\": 640,\n"
    "  \"visual_lod_radius_blocks\": 320,\n"
    "  \"speedup\": 30,\n"
    "  \"input_tick_ns\": 100000000,\n"
    "  \"movement_subcells_per_tick\": 5600,\n"
    "  \"platforms\": {\n"
    "    \"macos\": { \"framebuffer\": { \"width\": 2560, \"height\": 1440 }, \"present_mode\": \"fifo\" "
    "},\n"
    "    \"windows\": { \"framebuffer\": { \"width\": 1280, \"height\": 720 }, \"present_mode\": \"immed"
    "iate\" }\n"
    "  }\n"
    "}";
static_assert(GAME_BENCHMARK_S1_MOVEMENT_WINDOW / shared::TICK
    == GAME_BENCHMARK_S1_EXPECTED_MOVEMENT_UPDATES);

[[nodiscard]] bool gameBenchmarkS1RenderedCameraTrajectoryComplete(
    std::span<GameBenchmarkFrameSample const> frames
) noexcept;

[[nodiscard]] uint32_t gameBenchmarkS1PresentedCurrentColumnCount(
    client::TerrainLodScene const& scene,
    shared::PlayerPosition center
);
[[nodiscard]] uint32_t gameBenchmarkNativePresentedCurrentColumnCount(
    client::TerrainLodScene const& scene,
    shared::PlayerPosition center
);

struct GameBenchmarkPendingFrameSubmission final {
    uint64_t sequence_id = 0U;
    uint64_t swapchain_generation = 0U;
    uint32_t frame_slot = 0U;
    uint64_t submitted_offset_ns = 0U;
    std::optional<uint32_t> application_present_id;
    std::string submission_phase;
};

class GameBenchmarkFrameCompletionTracker final {
public:
    static constexpr uint32_t FRAME_SLOT_COUNT = 3U;

    [[nodiscard]] bool track(GameBenchmarkPendingFrameSubmission submission);
    void retireOtherGenerations(uint64_t current_swapchain_generation) noexcept;
    [[nodiscard]] std::optional<GameBenchmarkFrameCompletion> confirmFence(
        uint64_t observed_swapchain_generation,
        uint32_t observed_frame_slot,
        uint64_t completion_observed_offset_ns
    );
    [[nodiscard]] bool hasPendingPhase(std::string_view phase) const noexcept;

private:
    std::array<std::optional<GameBenchmarkPendingFrameSubmission>, FRAME_SLOT_COUNT> m_pending{};
};

class GameBenchmarkS1MovementWindow final {
public:
    [[nodiscard]] bool shouldPauseBeforeStart(
        bool playable_sphere_ready_observed,
        bool current_sphere_ready,
        bool trajectory_anchor_ready
    ) const noexcept;
    [[nodiscard]] std::chrono::nanoseconds routeElapsedAt(
        std::chrono::steady_clock::time_point sampled_at,
        std::optional<std::chrono::steady_clock::time_point> fill_route_started_at,
        bool playable_sphere_ready
    ) const noexcept;
    [[nodiscard]] bool observeSuccessfulInput(
        std::chrono::steady_clock::time_point sent_at,
        bool playable_sphere_ready
    ) noexcept;
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> startedAt() const noexcept;
    [[nodiscard]] bool contains(std::chrono::steady_clock::time_point sampled_at) const noexcept;

private:
    std::optional<std::chrono::steady_clock::time_point> m_started_at;
};

[[nodiscard]] shared::Direction gameBenchmarkS1MovementDirection(
    GameBenchmarkMovementRouteSample const& route,
    bool playable_sphere_ready
) noexcept;
void gameBenchmarkObserveGeometricSphere(
    GameBenchmarkFrameSample::PlayableSphere& sample,
    shared::SparseWorld const& world
);
[[nodiscard]] bool gameBenchmarkS1CurrentTerrainSampleReady(
    GameBenchmarkFrameSample::PlayableSphere const& sample
) noexcept;

[[nodiscard]] bool gameBenchmarkS1VisualLodSampleReady(
    std::optional<GameBenchmarkFrameSample::VisualLod> const& sample
) noexcept;
[[nodiscard]] bool gameBenchmarkNativeTerrainSampleCurrent(
    GameBenchmarkFrameSample::NativeTerrain const& sample,
    shared::TerrainIdentity const& session_identity,
    uint64_t session_join_epoch,
    uint64_t coverage_epoch
) noexcept;
[[nodiscard]] bool gameBenchmarkNativeTerrainFrameFenceConfirmed(
    GameBenchmarkFrameSample const& sample,
    std::span<GameBenchmarkFrameCompletion const> completions
) noexcept;
[[nodiscard]] bool gameBenchmarkS2NativeTurningAnchorReady(
    GameBenchmarkFrameSample const& frame,
    GameBenchmarkFrameCompletion const& completion,
    shared::TerrainSessionIdentity const& session,
    uint64_t coverage_epoch,
    shared::PlayerPosition const& player_position
);
[[nodiscard]] bool gameBenchmarkS2NativeRecoveryBarrierReady(
    GameBenchmarkFrameSample const& frame,
    GameBenchmarkFrameCompletion const& completion,
    shared::TerrainSessionIdentity const& session,
    uint64_t coverage_epoch,
    shared::PlayerPosition const& player_position
);
[[nodiscard]] bool gameBenchmarkS1MovementReceiptsComplete(GameBenchmarkEvidence const& benchmark) noexcept;
[[nodiscard]] bool gameBenchmarkS1ReadinessDeadlinesMet(
    std::optional<uint64_t> ready_prompt_offset_ns,
    std::optional<uint64_t> playable_ready_offset_ns
) noexcept;
[[nodiscard]] std::optional<GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate>
gameBenchmarkS1TrajectoryAnchorSample(
    GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate const& authoritative_state,
    GameBenchmarkFrameSample::PlayableSphere const& current_sphere,
    GameBenchmarkFrameSample::VisualLod const& visual_lod,
    uint64_t sampled_offset_ns
);

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
    explicit GameBenchmarkFullRadiusSequence(
        std::chrono::seconds warmup_timeout,
        bool defer_movement_window_until_input = false
    ) noexcept;
    void start(std::chrono::steady_clock::time_point started_at) noexcept;
    void observe(
        std::chrono::steady_clock::time_point sampled_at,
        std::optional<bool> exact_current_coverage,
        bool actual_playable_sphere_ready
    ) noexcept;
    void beginMovementWindow(std::chrono::steady_clock::time_point started_at) noexcept;
    [[nodiscard]] GameBenchmarkFullRadiusPhase phase() const noexcept { return m_phase; }
    [[nodiscard]] std::chrono::seconds warmupTimeout() const noexcept { return m_warmup_timeout; }
    [[nodiscard]] std::chrono::steady_clock::duration phaseElapsed(
        std::chrono::steady_clock::time_point sampled_at
    ) const noexcept;

private:
    void transition(GameBenchmarkFullRadiusPhase phase, std::chrono::steady_clock::time_point started_at) noexcept;

    std::chrono::seconds m_warmup_timeout;
    bool m_defer_movement_window_until_input = false;
    bool m_movement_window_started = false;
    GameBenchmarkFullRadiusPhase m_phase = GameBenchmarkFullRadiusPhase::InitialWarmup;
    std::chrono::steady_clock::time_point m_phase_started_at{};
    bool m_started = false;
};

[[nodiscard]] std::expected<std::chrono::seconds, std::string> gameBenchmarkFullRadiusWarmupTimeout(
    int64_t seconds
);

struct GameBenchmarkFrameBudgetWindow final {
    uint64_t start_offset_ns = 0U;
    uint64_t end_offset_ns = 0U;
    uint64_t required_duration_ns = 0U;
    std::string_view phase;
    bool complete = false;
    uint64_t completion_observation_tail_ns = 0U;
};

[[nodiscard]] GameBenchmarkNativeTurningCoverage gameBenchmarkS2NativeTurningCoverage(
    std::span<GameBenchmarkFrameSample const> frames,
    GameBenchmarkFrameBudgetWindow const& window,
    uint64_t session_join_epoch
);

[[nodiscard]]
std::vector<std::chrono::nanoseconds> gameBenchmarkSuccessfulPresentIntervals(
    std::span<GameBenchmarkFrameSample const> frames,
    std::string_view stationary_phase
);

[[nodiscard]] GameBenchmarkFrameBudgetEvidence gameBenchmarkFrameBudget(
    std::span<GameBenchmarkFrameSample const> frames,
    std::span<GameBenchmarkFrameCompletion const> fence_confirmed_frames,
    uint64_t target_frame_duration_ns,
    GameBenchmarkFrameBudgetWindow const& window,
    uint32_t required_in_budget_time_permille = 990U
);

[[nodiscard]] bool gameBenchmarkPresentationTimingComplete(
    GameBenchmarkPresentationTimingEvidence const& evidence
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

struct GameBenchmarkCaptureSite final {
    int32_t x;
    int32_t y;
    int32_t z;
    double yaw_degrees;
    double pitch_degrees;
};

struct GameBenchmarkOptions final {
    std::chrono::seconds cold_duration{ 2 };
    std::chrono::seconds warm_duration{ 3 };
    std::chrono::seconds uncapped_duration{ 1 };
    std::chrono::seconds deadline{ 60 };
    bool require_immediate_present_mode = false;
    GameBenchmarkWorkload workload = GameBenchmarkWorkload::OrdinaryMovement;
    uint32_t render_distance = shared::HEIGHT_TILE_INTEREST_RADIUS;
    std::string qualification_proof;
    shared::corelang::Backend backend = shared::corelang::Backend::Interpreter;
    std::chrono::seconds full_radius_warmup_timeout = GAME_BENCHMARK_FULL_RADIUS_DEFAULT_WARMUP_TIMEOUT;
    bool diagnostic_full_radius_capture = false;
    generation::TerrainModelTag terrain_model = generation::TerrainModelTag::R6;
    bool use_v10_fine_terrain = false;
    bool enable_live_ui = false;
    std::filesystem::path frame_capture_image;
    std::string frame_capture_phase = "playing";
    std::chrono::milliseconds frame_capture_delay{0};
    std::optional<int32_t> diagnostic_spawn_z;
    std::optional<GameBenchmarkCaptureSite> diagnostic_capture_site;
};

[[nodiscard]]
std::optional<shared::PlayerPosition> gameBenchmarkDiagnosticSpawnPosition(GameBenchmarkOptions const& options);

[[nodiscard]]
std::optional<std::string> gameBenchmarkSpawnOverrideError(GameBenchmarkOptions const& options);

[[nodiscard]]
std::optional<std::string> gameBenchmarkCaptureSiteError(GameBenchmarkOptions const& options);

[[nodiscard]]
std::function<std::optional<client::CameraAngles>(std::chrono::steady_clock::time_point)>
gameBenchmarkDiagnosticCaptureCameraOverride(GameBenchmarkOptions const& options);

[[nodiscard]]
bool gameBenchmarkSourceStreamingWorkload(GameBenchmarkOptions const& options) noexcept;

[[nodiscard]]
std::optional<GameBenchmarkNativeTerrainDiagnostics> gameBenchmarkNativeTerrainDiagnostics(
    std::span<GameBenchmarkFrameSample const> frames,
    std::span<GameBenchmarkFrameCompletion const> completions
);

[[nodiscard]] std::chrono::seconds gameBenchmarkDeadline(GameBenchmarkOptions const& options);

[[nodiscard]]
bool gameBenchmarkDiagnosticOverridesPresent(bool explicit_diagnostic_capture = false) noexcept;

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
