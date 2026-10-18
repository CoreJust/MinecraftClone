#pragma once

#include "Camera.hpp"
#include "GameClient.hpp"
#include "PlayerPresentation.hpp"
#include "PlayerPreviewLod.hpp"
#include "PreviewMeshing.hpp"
#include "PreviewResidency.hpp"
#include "render/DebugHud.hpp"
#include "render/InstalledShaderAssets.hpp"
#include "render/VulkanRenderer.hpp"

#include <shared/ProjectInfo.hpp>
#include <shared/net/Message.hpp>
#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <core/platform/glfw/GlfwWindow.hpp>

#include <glm/vec3.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace client {

enum class PlayerClientCapturePreset : uint8_t {
    CentralSpike,
    TrenchFirstSpike,
    MountainClimb,
    FirstPersonOrigin,
};

struct PlayerClientCapturePlan final {
    PlayerClientCapturePreset preset;
    std::string_view name;
    std::string_view title;
    CameraPerspective perspective;
    CameraAngles look_angles;
    shared::PlayerPosition target;
    std::optional<shared::MovementCapabilities> final_movement_capabilities;
    double vertical_fov_degrees = CameraProjection{}.vertical_fov_degrees;
    double rear_camera_distance = MAX_LOCAL_PLAYER_CAMERA_DISTANCE;
};

[[nodiscard]] std::optional<PlayerClientCapturePreset> parsePlayerClientCapturePreset(std::string_view value) noexcept;
[[nodiscard]] PlayerClientCapturePlan playerClientCapturePlan(PlayerClientCapturePreset preset) noexcept;
[[nodiscard]] std::array<PlayerClientCapturePreset, 4> playerClientCapturePresets() noexcept;
[[nodiscard]] uint16_t playerClientCaptureSpeedup(int64_t distance_subcells) noexcept;
[[nodiscard]] std::chrono::milliseconds playerClientRendererFrameBudget(bool capture_enabled) noexcept;
[[nodiscard]] std::chrono::steady_clock::time_point playerClientPreviewMeshWorkDeadline(
    std::chrono::steady_clock::time_point renderer_deadline,
    std::chrono::steady_clock::time_point mesh_work_started_at) noexcept;
[[nodiscard]] uint32_t playerClientHeightTileChangeBudget(bool capture_enabled) noexcept;
[[nodiscard]] uint32_t playerClientPreviewMeshJobBudget(bool capture_enabled) noexcept;
[[nodiscard]] uint32_t playerClientPreviewMeshBacklogCapacity(bool capture_enabled) noexcept;
[[nodiscard]] uint32_t playerClientPreviewMeshAdmissionBudget(bool capture_enabled) noexcept;
[[nodiscard]] bool playerClientShouldQueuePreviewRemoval(bool capture_enabled, bool mesh_visible) noexcept;
[[nodiscard]] uint32_t playerClientPreviewRemovalBudget() noexcept;
[[nodiscard]] bool playerClientShouldRemovePreviewMesh(bool currently_interested) noexcept;
[[nodiscard]] uint32_t playerClientCaptureLogicalSize(
    uint32_t logical_size,
    uint32_t framebuffer_size,
    uint32_t target_framebuffer_size
) noexcept;
[[nodiscard]]
bool playerClientCaptureExtentMeetsMinimum(uint32_t width, uint32_t height) noexcept;

struct PlayerClientCaptureOptions final {
    std::filesystem::path image_path;
    PlayerClientCapturePreset preset = PlayerClientCapturePreset::CentralSpike;
    uint32_t minimum_height_tile_meshes = 0U;
    std::chrono::milliseconds readiness_deadline{ std::chrono::minutes{ 15 } };
    std::chrono::milliseconds sweep_deadline{ std::chrono::minutes{ 5 } };
};

struct PlayerClientBenchmarkOptions final {
    bool require_immediate_present_mode = false;
    bool freeze_camera = false;
    bool show_debug_hud = false;
    bool enable_mesh_stage_diagnostics = false;
};

struct PlayerClientMeshResultDrainDiagnostics final {
    uint32_t drained_result_count = 0U;
    uint32_t stale_epoch_discard_count = 0U;
    uint32_t no_longer_interested_or_resident_discard_count = 0U;
    uint32_t source_mismatch_requeue_count = 0U;
    uint32_t target_detail_mismatch_requeue_count = 0U;
    uint32_t cannot_publish_requeue_count = 0U;
    uint32_t publish_attempt_count = 0U;
    uint32_t publish_success_count = 0U;
    uint32_t publish_failure_count = 0U;
    std::chrono::nanoseconds seam_planning_duration{ 0 };
    std::chrono::nanoseconds renderer_batch_upsert_duration{ 0 };
    client::RendererHeightTileUpsertDiagnostics renderer_upsert_stages;
};

struct PlayerClientRenderDiagnostics final {
    std::chrono::nanoseconds window_input_duration{ 0 };
    std::chrono::nanoseconds height_tile_interest_duration{ 0 };
    std::chrono::nanoseconds preview_lod_refresh_duration{ 0 };
    uint32_t preview_lod_classified_bucket_count = 0U;
    uint32_t preview_lod_evaluated_tile_count = 0U;
    uint32_t preview_lod_changed_tile_count = 0U;
    std::chrono::nanoseconds preview_mesh_pump_before_changes_duration{ 0 };
    std::chrono::nanoseconds height_tile_change_and_removal_duration{ 0 };
    std::chrono::nanoseconds height_tile_change_queue_duration{ 0 };
    std::chrono::nanoseconds preview_mesh_remove_duration{ 0 };
    std::chrono::nanoseconds preview_mesh_remove_renderer_duration{ 0 };
    std::chrono::nanoseconds preview_mesh_remove_neighbor_enqueue_duration{ 0 };
    uint32_t preview_mesh_remove_count = 0U;
    std::chrono::nanoseconds preview_mesh_pump_after_changes_duration{ 0 };
    std::chrono::nanoseconds camera_resolution_duration{ 0 };
    std::chrono::nanoseconds before_resize_duration{ 0 };
    std::chrono::nanoseconds resize_duration{ 0 };
    std::chrono::nanoseconds after_resize_duration{ 0 };
    std::chrono::nanoseconds renderer_call_duration{ 0 };
    std::chrono::nanoseconds after_renderer_duration{ 0 };
};

inline constexpr int32_t PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS = 7;
inline constexpr uint32_t PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_TILE_COUNT = 225U;
struct PlayerClientPreviewMeshProbeOffset final {
    int32_t x = 0;
    int32_t y = 0;
};

inline constexpr auto PLAYER_CLIENT_PREVIEW_MESH_PROBE_OFFSETS = [] {
    std::array<PlayerClientPreviewMeshProbeOffset, PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_TILE_COUNT> offsets{};
    uint32_t index = 0U;
    offsets[index++] = {};
    for (int32_t radius = 1; radius <= PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS; ++radius) {
        for (int32_t x = -radius; x < radius; ++x) {
            offsets[index++] = { x, -radius };
        }
        for (int32_t y = -radius; y < radius; ++y) {
            offsets[index++] = { radius, y };
        }
        for (int32_t x = radius; x > -radius; --x) {
            offsets[index++] = { x, radius };
        }
        for (int32_t y = radius; y > -radius; --y) {
            offsets[index++] = { -radius, y };
        }
    }
    return offsets;
}();
// Covers the next x200 movement tick, including tile-flooring error and the
// farthest corner of the protected near square.
inline constexpr uint32_t PLAYER_CLIENT_PREVIEW_MESH_PREWARM_SPEEDUP = 200U;
inline constexpr int32_t PLAYER_CLIENT_PREVIEW_MESH_PREWARM_RADIUS = [] {
    uint32_t const tile_subcells = shared::SUBCELLS_PER_CELL * shared::HEIGHT_TILE_SIDE_LENGTH;
    uint32_t const maximum_movement_tiles =
        (static_cast<uint32_t>(shared::MOVEMENT_SUBCELLS_PER_TICK) * PLAYER_CLIENT_PREVIEW_MESH_PREWARM_SPEEDUP +
            tile_subcells - 1U) /
        tile_subcells;
    constexpr uint32_t TILE_CENTER_ROUNDING_BOUND_TILES = 2U;
    uint32_t const near_corner_distance_squared =
        2U * PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS * PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS;
    uint32_t near_corner_radius = 0U;
    while (near_corner_radius * near_corner_radius < near_corner_distance_squared) {
        ++near_corner_radius;
    }
    return static_cast<int32_t>(maximum_movement_tiles + TILE_CENTER_ROUNDING_BOUND_TILES + near_corner_radius);
}();
inline constexpr uint32_t PLAYER_CLIENT_PREVIEW_MESH_PREWARM_OFFSET_COUNT = [] {
    uint32_t count = 0U;
    int64_t const maximum_distance_squared =
        static_cast<int64_t>(PLAYER_CLIENT_PREVIEW_MESH_PREWARM_RADIUS) * PLAYER_CLIENT_PREVIEW_MESH_PREWARM_RADIUS;
    for (int32_t y = -PLAYER_CLIENT_PREVIEW_MESH_PREWARM_RADIUS; y <= PLAYER_CLIENT_PREVIEW_MESH_PREWARM_RADIUS; ++y) {
        for (int32_t x = -PLAYER_CLIENT_PREVIEW_MESH_PREWARM_RADIUS; x <= PLAYER_CLIENT_PREVIEW_MESH_PREWARM_RADIUS;
            ++x) {
            int64_t const distance_squared = static_cast<int64_t>(x) * x + static_cast<int64_t>(y) * y;
            bool const outside_near_square = x < -PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS ||
                x > PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS || y < -PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS ||
                y > PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS;
            if (distance_squared <= maximum_distance_squared && outside_near_square) {
                ++count;
            }
        }
    }
    return count;
}();
inline constexpr auto PLAYER_CLIENT_PREVIEW_MESH_PREWARM_OFFSETS = [] {
    std::array<PlayerClientPreviewMeshProbeOffset, PLAYER_CLIENT_PREVIEW_MESH_PREWARM_OFFSET_COUNT> offsets{};
    uint32_t index = 0U;
    int64_t const maximum_distance_squared =
        static_cast<int64_t>(PLAYER_CLIENT_PREVIEW_MESH_PREWARM_RADIUS) * PLAYER_CLIENT_PREVIEW_MESH_PREWARM_RADIUS;
    for (int32_t y = -PLAYER_CLIENT_PREVIEW_MESH_PREWARM_RADIUS; y <= PLAYER_CLIENT_PREVIEW_MESH_PREWARM_RADIUS; ++y) {
        for (int32_t x = -PLAYER_CLIENT_PREVIEW_MESH_PREWARM_RADIUS; x <= PLAYER_CLIENT_PREVIEW_MESH_PREWARM_RADIUS;
            ++x) {
            int64_t const distance_squared = static_cast<int64_t>(x) * x + static_cast<int64_t>(y) * y;
            bool const outside_near_square = x < -PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS ||
                x > PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS || y < -PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS ||
                y > PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS;
            if (distance_squared <= maximum_distance_squared && outside_near_square) {
                offsets[index++] = { x, y };
            }
        }
    }
    return offsets;
}();
inline constexpr uint32_t PLAYER_CLIENT_BENCHMARK_FULL_TERRAIN_RADIAL_BAND_WIDTH = 16U;
inline constexpr size_t PLAYER_CLIENT_BENCHMARK_FULL_TERRAIN_RADIAL_BAND_COUNT = 16U;
inline constexpr uint32_t PLAYER_CLIENT_BENCHMARK_FULL_TERRAIN_DETAIL_COUNT = 5U;

// Current-center sample; a visible mesh counts only when it uses the current resident tile handle.
struct PlayerClientBenchmarkTerrainTileState final {
    shared::HeightTileKey key{};
    bool desired = false;
    bool resident = false;
    bool current_mesh_visible = false;
    bool pending = false;
    bool active = false;
};

struct PlayerClientBenchmarkTerrainInfo final {
    std::optional<shared::HeightTileKey> center;
    std::array<PlayerClientBenchmarkTerrainTileState, PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_TILE_COUNT>
        near_tiles{};
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

struct PlayerClientBenchmarkFullTerrainInfo final {
    std::optional<shared::HeightTileKey> center;
    bool scan_performed = false;
    bool potentially_ready = false;
    std::optional<bool> exact_current_coverage;
    uint32_t desired_count = 0U;
    uint32_t resident_total_count = 0U;
    uint32_t visible_mesh_count = 0U;
    uint32_t renderer_mesh_count = 0U;
    uint32_t renderer_face_count = 0U;
    uint32_t renderer_draw_face_count = 0U;
    uint32_t renderer_stone_face_capacity = 0U;
    uint32_t pending_delivery_batch_count = 0U;
    uint32_t pending_residency_change_count = 0U;
    uint32_t pending_mesh_count = 0U;
    uint32_t active_mesh_job_count = 0U;
    uint32_t dirty_mesh_job_count = 0U;
    uint32_t worker_result_count = 0U;
    uint32_t worker_outstanding_count = 0U;
    uint32_t pending_removal_count = 0U;
    std::optional<uint32_t> resident_desired_count;
    std::optional<uint32_t> nonresident_desired_count;
    std::optional<uint32_t> current_handle_mesh_visible_count;
    std::optional<uint32_t> stale_or_missing_mesh_count;
    std::optional<uint32_t> unclassified_resident_missing_count;
    std::optional<uint32_t> visible_outside_interest_count;
    std::optional<std::array<uint32_t, PLAYER_CLIENT_BENCHMARK_FULL_TERRAIN_DETAIL_COUNT>>
        current_visible_mesh_detail_counts;
    std::optional<uint32_t> unclassified_current_visible_mesh_detail_count;
    struct RadialBand final {
        uint32_t desired_count = 0U;
        uint32_t resident_count = 0U;
        uint32_t nonresident_count = 0U;
        uint32_t current_mesh_visible_count = 0U;
        uint32_t stale_or_missing_mesh_count = 0U;
    };
    std::array<RadialBand, PLAYER_CLIENT_BENCHMARK_FULL_TERRAIN_RADIAL_BAND_COUNT> radial_bands{};
};

struct PlayerHeightTileKeyHash final {
    [[nodiscard]] size_t operator()(shared::HeightTileKey key) const noexcept;
};

class PlayerPreviewMeshCoverage final {
public:
    void clear() noexcept { m_visible_count = 0U; }
    void interestAdded(bool already_visible) noexcept;
    void interestRemoved(bool visible);
    void meshPublished(bool newly_visible, bool interested) noexcept;
    [[nodiscard]] uint32_t visibleCount() const noexcept { return m_visible_count; }
    [[nodiscard]] bool complete(uint32_t interest_count) const noexcept
    {
        return interest_count != 0U && m_visible_count == interest_count;
    }

private:
    uint32_t m_visible_count = 0U;
};

class PlayerPreviewMeshAdmissionWindow final {
public:
    explicit PlayerPreviewMeshAdmissionWindow(uint32_t capacity) noexcept
        : m_capacity(capacity)
    {}

    [[nodiscard]] bool canAccept() const noexcept { return m_outstanding < m_capacity; }
    [[nodiscard]] bool tryAdmit() noexcept;
    [[nodiscard]] bool releaseOne() noexcept;
    [[nodiscard]] uint32_t outstandingCount() const noexcept { return m_outstanding; }

private:
    uint32_t const m_capacity;
    uint32_t m_outstanding = 0U;
};

[[nodiscard]] bool playerClientCaptureCoverageComplete(
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> const& interest,
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> const& visible_meshes,
    PreviewResidency const& residency,
    uint32_t expected_tile_count,
    size_t pending_preview_removals
) noexcept;

class PlayerPreviewMeshQueue final {
public:
    struct StorageStats final {
        uint64_t retained_bytes = 0U;
        uint64_t buffer_reuses = 0U;
        uint64_t ticket_reuses = 0U;
    };
    PlayerPreviewMeshQueue();
    ~PlayerPreviewMeshQueue();
    PlayerPreviewMeshQueue(PlayerPreviewMeshQueue const&) = delete;
    PlayerPreviewMeshQueue& operator=(PlayerPreviewMeshQueue const&) = delete;
    void clear() noexcept;
    void reserveStorage(uint32_t maximum_pending);
    [[nodiscard]] StorageStats storageStats() const noexcept;
    [[nodiscard]] bool push(shared::HeightTileKey key);
    void pushBatch(std::span<shared::HeightTileKey const> keys);
    void resetPriority(
        shared::HeightTileKey center,
        int8_t heading_x,
        int8_t heading_y,
        std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> const& interest
    );
    void beginPriorityRefresh(shared::HeightTileKey center, int8_t heading_x, int8_t heading_y);
    // Counts inspected unranked entries and ready heads, including stale removal.
    [[nodiscard]]
    uint32_t refreshPriority(
        uint32_t maximum_entries,
        std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> const& interest
    );
    [[nodiscard]] bool contains(shared::HeightTileKey key) const noexcept;
    [[nodiscard]]
    std::optional<shared::HeightTileKey> nearestPending(
        std::span<shared::HeightTileKey const> candidates
    ) const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool hasReady() const noexcept;
    // Consumes only the caller's remaining stale-head budget; unranked work stays deferred.
    [[nodiscard]] bool prepareReady(uint32_t& maintenance_budget) noexcept;
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] shared::HeightTileKey top() const noexcept;
    [[nodiscard]]
    bool erase(shared::HeightTileKey key) noexcept;
    void pop();

private:
    class RetirementState;
    struct Entry final {
        double priority;
        shared::HeightTileKey key;
        uint64_t ticket;
    };
    [[nodiscard]] Entry entryFor(shared::HeightTileKey key, uint64_t ticket) const noexcept;
    [[nodiscard]] bool isCurrent(Entry const& entry) const noexcept;
    [[nodiscard]] uint32_t pruneReadyHeap(uint32_t maximum_entries) noexcept;
    [[nodiscard]] static bool lowerPriority(Entry const& first, Entry const& second) noexcept;
    void ensureHeapCapacity(uint64_t required_entries);

    std::vector<Entry> m_heap;
    std::deque<std::vector<Entry>> m_unranked;
    std::unordered_map<shared::HeightTileKey, uint64_t, PlayerHeightTileKeyHash> m_tickets;
    std::unique_ptr<RetirementState> m_retirement;
    uint64_t m_next_ticket = 1U;
    shared::HeightTileKey m_center{};
    int8_t m_heading_x = 0;
    int8_t m_heading_y = 127;
};

struct PlayerPreviewMeshAdmissionCandidate final {
    shared::HeightTileKey key{};
    bool current_near = false;
    bool forecast_near = false;
};

[[nodiscard]] uint8_t playerClientPreviewMeshPriorityTier(
    shared::HeightTileKey center,
    std::optional<shared::HeightTileKey> forecast_center,
    shared::HeightTileKey key
) noexcept;

[[nodiscard]]
std::optional<PlayerPreviewMeshAdmissionCandidate> nextPlayerPreviewMeshAdmissionCandidate(
    PlayerPreviewMeshQueue &pending,
    uint32_t& maintenance_budget,
    std::span<shared::HeightTileKey const> nearby_candidates,
    bool nearby_probe_complete,
    std::span<shared::HeightTileKey const> prewarm_candidates,
    bool prewarm_probe_complete,
    std::span<shared::HeightTileKey const> forecast_candidates = {},
    bool forecast_probe_complete = true);

class PlayerClient final : public GameClient {
public:
    explicit PlayerClient(
        shared::WorldMode mode = shared::WorldMode::Flat,
        std::optional<PlayerClientCaptureOptions> capture = std::nullopt,
        std::optional<PlayerClientBenchmarkOptions> benchmark = std::nullopt
    );
    ~PlayerClient();
    [[nodiscard]] RendererRuntimeInfo benchmarkRuntimeInfo() const;
    [[nodiscard]] PlayerClientRenderDiagnostics benchmarkRenderDiagnostics() const noexcept
    {
        return m_render_diagnostics;
    }
    // On-demand so ordinary render frames do not pay for the bounded terrain-state probe.
    [[nodiscard]] PlayerClientBenchmarkTerrainInfo benchmarkTerrainStreamingInfo() const;
    // The exact key scan is opt-in; readiness polling reads only aggregate state.
    [[nodiscard]] PlayerClientBenchmarkFullTerrainInfo benchmarkFullTerrainStreamingInfo(
        bool scan_exact_coverage = false
    ) const;
    [[nodiscard]] bool captureSucceeded() const noexcept { return m_capture_succeeded; }
private:
    friend struct PlayerClientTestAccess;
    class PreviewMeshWorkerPool;
    class PreviewCacheRetirementReaper;
    struct PreviewTileElevation final {
        HeightTileRevision tile_revision{};
        uint64_t tile_token = 0U;
        std::array<uint64_t, 4> neighbor_tokens{};
        std::array<shared::HeightTileSurfaceDetail, 4> neighbor_details{};
        std::array<uint64_t, 4> neighbor_profile_tokens{};
        std::array<std::optional<shared::HeightTileSurfaceElevationRange>, 4> neighbor_mesh_ranges{};
        uint16_t minimum = 0U;
        uint16_t maximum = 0U;
        shared::HeightTileSurfaceFaceAreas face_areas;
    };
    struct PreviewMeshEdgeProfiles final {
        std::array<shared::HeightTileSurfaceEdgeProfile, 4> edges{};
        std::array<uint64_t, 4> tokens{};
    };
    shared::Direction input() override;
    void servicePlatformEvents() override;
    void render() override;
    [[nodiscard]] bool presentationSucceeded() const override;
    void onConnectionStateReset() override;
private:
    void beginContinuousLook() noexcept;
    void updateFlightControlToggles();
    void refreshHeightTileInterest(
        shared::Player const& player,
        shared::HeightTileSurfaceProjection projection = {},
        bool capture_diagnostics = false);
    [[nodiscard]] shared::HeightTileSurfaceDetail previewMeshDetail(
        shared::HeightTileKey key,
        HeightTileHandle const& tile
    );
    [[nodiscard]] std::array<HeightTileHandle, 4> previewMeshNeighbors(shared::HeightTileKey key) const;
    void refreshPreviewMeshNeighborElevations(shared::HeightTileKey key);
    [[nodiscard]] std::array<shared::HeightTileSurfaceDetail, 4> previewMeshNeighborDetails(
        shared::HeightTileKey key
    ) const;
    [[nodiscard]] std::array<uint64_t, 4> previewMeshNeighborProfileTokens(shared::HeightTileKey key) const;
    [[nodiscard]] bool enqueuePreviewMeshJob(
        shared::HeightTileKey key,
        HeightTileHandle const& tile,
        shared::HeightTileKey center,
        std::span<shared::HeightTileKey const> nearby_candidates,
        std::chrono::steady_clock::time_point near_admission_deadline
    );
    [[nodiscard]]
    double maximumUnobstructedCameraDistance(
        PlayerPresentationPosition const& local_position
    ) const noexcept;
    void queuePreviewMesh(shared::HeightTileKey key);
    void queuePreviewMeshes(std::span<shared::HeightTileKey const> keys);
    void queuePreviewRemoval(shared::HeightTileKey key);
    void markPreviewMeshVisible(shared::HeightTileKey key);
    [[nodiscard]] bool publishPreviewMesh(
        shared::HeightTileKey key,
        shared::HeightTileSurfaceMesh &mesh,
        HeightTileHandle const& tile,
        std::chrono::steady_clock::time_point deadline
    );
    void removePreviewMesh(shared::HeightTileKey key, std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] bool refreshVisiblePreviewMesh(
        shared::HeightTileKey key,
        std::chrono::steady_clock::time_point deadline,
        PreviewMeshSeamBridgeSet const* candidate_bridges = nullptr
    );
    void processPendingPreviewMeshes(
        uint32_t maximum_meshes,
        std::chrono::steady_clock::time_point mesh_work_deadline);
    void submitPreviewCacheRetirements() noexcept;
    void drainPreviewCacheRetirements() noexcept;
    [[nodiscard]] uint64_t previewCacheRetirementHighWaterBytes() const noexcept;
    [[nodiscard]] uint64_t previewCacheRetirementOutstandingBytes() const noexcept;
    [[nodiscard]] uint64_t previewCacheRetirementFallbackCount() const noexcept;
    [[nodiscard]] uint64_t previewCacheRetirementSubmissionCount() const noexcept;
    [[nodiscard]] bool hasCurrentPreviewMeshCoverage() const noexcept;
    [[nodiscard]] bool hasExactCurrentPreviewMeshCoverage() const noexcept;
    [[nodiscard]] bool hasExactCurrentRendererMeshCoverage() const;
    [[nodiscard]] shared::Direction captureInput() noexcept;
    [[nodiscard]] bool captureAtTarget(shared::Player const& player) const noexcept;
    [[nodiscard]] bool captureSceneReady() const;
    void failCapture(std::string_view reason);
    Camera m_camera{
        { .position = { 9.0, 9.0, 13.0 } },
    };
    Camera m_look_camera{
        { .position = { 9.0, 9.0, 13.0 } },
    };
    std::optional<PlayerClientCaptureOptions> m_capture;
    std::optional<PlayerClientBenchmarkOptions> m_benchmark;
    PlayerClientRenderDiagnostics m_render_diagnostics{};
    bool m_last_presentation_succeeded = false;
    core::platform::glfw::GlfwWindow m_window;
    InstalledShaderAssets m_shader_assets;
    VulkanRenderer m_renderer;
    std::vector<PlayerRenderData> m_render_data;
    DebugHudToggleLatch m_debug_hud_toggle;
    DebugHudToggleLatch m_speedup_increase_latch;
    DebugHudToggleLatch m_speedup_decrease_latch;
    DebugHudToggleLatch m_acceleration_toggle_latch;
    DebugHudToggleLatch m_camera_perspective_latch;
    DebugHudToggleLatch m_movement_capability_latch;
    CameraPerspective m_camera_perspective = CameraPerspective::FirstPerson;
    size_t m_speedup_profile_index = 2U;
    bool m_acceleration_enabled = false;
    bool m_jump_queued = false;
    double m_last_cursor_x = 0.0;
    double m_last_cursor_y = 0.0;
    bool m_has_cursor_position = false;
    bool m_was_reload_pressed = false;
    int8_t m_interest_heading_x = 0;
    int8_t m_interest_heading_y = 127;
    int8_t m_applied_interest_heading_x = 0;
    int8_t m_applied_interest_heading_y = 0;
    std::optional<shared::HeightTileKey> m_height_tile_center;
    shared::HeightTileInterestDelta m_interest_delta_scratch;
    std::vector<shared::HeightTileKey> m_remesh_closure_scratch;
    std::vector<shared::HeightTileKey> m_pending_keys_scratch;
    uint32_t m_interest_storage_radius = 0U;
    std::optional<shared::HeightTileKey> m_preview_forecast_center;
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> m_height_tile_interest;
    std::deque<shared::HeightTileKey> m_pending_preview_removals;
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> m_pending_preview_removal_set;
    PlayerPreviewMeshQueue m_pending_preview_meshes;
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> m_preview_mesh_jobs;
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> m_preview_mesh_dirty_jobs;
    std::optional<std::chrono::steady_clock::time_point> m_deferred_preview_mesh_result_deadline;
    PlayerClientMeshResultDrainDiagnostics m_mesh_result_drain_diagnostics{};
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> m_visible_preview_meshes;
    PlayerPreviewMeshCoverage m_preview_mesh_coverage;
    std::unordered_map<shared::HeightTileKey, HeightTileHandle, PlayerHeightTileKeyHash>
        m_visible_preview_mesh_tiles;
    std::unordered_map<shared::HeightTileKey, shared::HeightTileSurfaceMesh, PlayerHeightTileKeyHash>
        m_visible_preview_mesh_bases;
    std::unordered_map<
        shared::HeightTileKey,
        PreviewMeshEdgeProfiles,
        PlayerHeightTileKeyHash
    > m_visible_preview_mesh_edge_profiles;
    std::unordered_map<shared::HeightTileKey, PreviewMeshSeamBridgeSet, PlayerHeightTileKeyHash>
        m_visible_preview_mesh_seam_bridges;
    std::unique_ptr<PreviewCacheRetirementReaper> m_preview_cache_retirement_reaper;
    PlayerPreviewLod m_preview_lod;
    std::unordered_map<shared::HeightTileKey, PreviewTileElevation, PlayerHeightTileKeyHash>
        m_preview_tile_elevations;
    shared::HeightTileSurfaceProjection m_preview_mesh_projection{};
    std::unique_ptr<PreviewMeshWorkerPool> m_preview_mesh_workers;
    uint64_t m_benchmark_accepted_near_admissions = 0U;
    uint64_t m_benchmark_accepted_far_admissions = 0U;
    uint64_t m_benchmark_far_admissions_while_near_pending = 0U;
    uint64_t m_preview_mesh_epoch = 1U;
    uint64_t m_preview_edge_profile_token = 0U;
    bool m_capture_requested = false;
    bool m_capture_label_initialized = false;
    std::optional<std::chrono::steady_clock::time_point> m_capture_started_at;
    std::optional<std::chrono::steady_clock::time_point> m_capture_rotation_started_at;
    std::optional<std::chrono::steady_clock::time_point> m_capture_last_progress_at;
    uint64_t m_capture_frames = 0U;
    uint64_t m_capture_height_changes = 0U;
    uint64_t m_capture_mesh_results = 0U;
    uint64_t m_capture_mesh_publish_failures = 0U;
    uint64_t m_capture_mesh_drain_micros = 0U;
    uint64_t m_capture_mesh_reprioritize_micros = 0U;
    std::optional<uint8_t> m_capture_last_capability_cycle_source;
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> m_capture_interest_baseline;
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> m_capture_mesh_baseline;
    uint32_t m_capture_rotation_step = 0U;
    bool m_capture_rotation_started = false;
    bool m_capture_succeeded = false;
    bool m_capture_failed = false;
};

} // namespace client
