#pragma once

#include "Camera.hpp"
#include "GameClient.hpp"
#include "PlayerPreviewLod.hpp"
#include "PreviewMeshing.hpp"
#include "render/InstalledShaderAssets.hpp"
#include "render/VulkanRenderer.hpp"

#include <shared/ProjectInfo.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <core/platform/glfw/GlfwWindow.hpp>

#include <array>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
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
[[nodiscard]] uint32_t playerClientHeightTileChangeBudget(bool capture_enabled) noexcept;
[[nodiscard]] uint32_t playerClientPreviewMeshJobBudget(bool capture_enabled) noexcept;
[[nodiscard]] bool playerClientShouldQueuePreviewRemoval(bool capture_enabled, bool mesh_visible) noexcept;
[[nodiscard]] uint32_t playerClientPreviewRemovalBudget(bool capture_enabled, bool current_coverage) noexcept;
[[nodiscard]] uint32_t playerClientCaptureLogicalSize(
    uint32_t logical_size,
    uint32_t framebuffer_size,
    uint32_t target_framebuffer_size
) noexcept;

struct PlayerClientCaptureOptions final {
    std::filesystem::path image_path;
    PlayerClientCapturePreset preset = PlayerClientCapturePreset::CentralSpike;
    uint32_t minimum_height_tile_meshes = shared::HEIGHT_TILE_INTEREST_COUNT;
    std::chrono::milliseconds readiness_deadline{ std::chrono::minutes{ 15 } };
    std::chrono::milliseconds sweep_deadline{ std::chrono::minutes{ 5 } };
};

struct PlayerClientBenchmarkOptions final {
    bool require_immediate_present_mode = false;
    bool freeze_camera = false;
};

struct PlayerHeightTileKeyHash final {
    [[nodiscard]] size_t operator()(shared::HeightTileKey key) const noexcept;
};

class PlayerPreviewMeshCoverage final {
public:
    void clear() noexcept { m_visible_count = 0U; }
    void interestAdded(bool already_visible) noexcept;
    void interestRemoved(bool visible) noexcept;
    void meshPublished(bool newly_visible, bool interested) noexcept;
    [[nodiscard]] uint32_t visibleCount() const noexcept { return m_visible_count; }
    [[nodiscard]] bool complete(uint32_t interest_count) const noexcept
    {
        return interest_count != 0U && m_visible_count == interest_count;
    }

private:
    uint32_t m_visible_count = 0U;
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
    void clear() noexcept;
    [[nodiscard]] bool push(shared::HeightTileKey key);
    void resetPriority(
        shared::HeightTileKey center,
        int8_t heading_x,
        int8_t heading_y,
        std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> const& interest
    );
    void beginPriorityRefresh(shared::HeightTileKey center, int8_t heading_x, int8_t heading_y);
    [[nodiscard]]
    uint32_t refreshPriority(
        uint32_t maximum_entries,
        std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> const& interest
    );
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool hasReady() const noexcept;
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] shared::HeightTileKey top() const noexcept;
    void pop();

private:
    struct Entry final {
        double priority;
        shared::HeightTileKey key;
    };
    [[nodiscard]] Entry entryFor(shared::HeightTileKey key) const noexcept;
    [[nodiscard]] static bool lowerPriority(Entry const& first, Entry const& second) noexcept;

    std::vector<Entry> m_heap;
    std::deque<std::vector<Entry>> m_unranked;
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> m_keys;
    shared::HeightTileKey m_center{};
    int8_t m_heading_x = 0;
    int8_t m_heading_y = 127;
};

class PlayerClient final : public GameClient {
public:
    explicit PlayerClient(
        shared::WorldMode mode = shared::WorldMode::Flat,
        std::optional<PlayerClientCaptureOptions> capture = std::nullopt,
        std::optional<PlayerClientBenchmarkOptions> benchmark = std::nullopt
    );
    ~PlayerClient();
    [[nodiscard]] RendererRuntimeInfo benchmarkRuntimeInfo() const;
    [[nodiscard]] bool captureSucceeded() const noexcept { return m_capture_succeeded; }
private:
    friend struct PlayerClientTestAccess;
    class PreviewMeshWorkerPool;
    struct PreviewTileElevation final {
        HeightTileRevision tile_revision{};
        uint64_t tile_token = 0U;
        std::array<uint64_t, 4> neighbor_tokens{};
        std::array<std::optional<shared::HeightTileSurfaceElevationRange>, 4> neighbor_mesh_ranges{};
        uint16_t minimum = 0U;
        uint16_t maximum = 0U;
    };
    shared::Direction input() override;
    void render() override;
    [[nodiscard]] bool presentationSucceeded() const override;
    void onConnectionStateReset() override;
private:
    void beginContinuousLook() noexcept;
    void updateFlightControlToggles();
    void refreshHeightTileInterest(
        shared::Player const& player,
        shared::HeightTileSurfaceProjection projection = {}
    );
    [[nodiscard]] shared::HeightTileSurfaceDetail previewMeshDetail(
        shared::HeightTileKey key,
        HeightTileHandle const& tile
    );
    [[nodiscard]] std::array<HeightTileHandle, 4> previewMeshNeighbors(shared::HeightTileKey key) const;
    void refreshPreviewMeshNeighborElevations(shared::HeightTileKey key);
    [[nodiscard]] std::array<shared::HeightTileSurfaceDetail, 4> previewMeshNeighborDetails(
        shared::HeightTileKey key
    );
    [[nodiscard]]
    double maximumUnobstructedCameraDistance(
        PlayerPresentationPosition const& local_position
    ) const noexcept;
    void queuePreviewMesh(shared::HeightTileKey key);
    void queuePreviewRemoval(shared::HeightTileKey key);
    void markPreviewMeshVisible(shared::HeightTileKey key);
    void publishPreviewMesh(
        shared::HeightTileKey key,
        shared::HeightTileSurfaceMesh mesh,
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
        std::chrono::steady_clock::time_point deadline
    );
    [[nodiscard]] bool hasCurrentPreviewMeshCoverage() const noexcept;
    [[nodiscard]] bool hasExactCurrentPreviewMeshCoverage() const noexcept;
    [[nodiscard]] bool hasExactCurrentRendererMeshCoverage() const;
    [[nodiscard]] shared::Direction captureInput() noexcept;
    [[nodiscard]] bool captureAtTarget(shared::Player const& player) const noexcept;
    [[nodiscard]] bool captureSceneReady() const;
    void failCapture(std::string_view reason) noexcept;
    Camera m_camera{
        { .position = { 9.0, 9.0, 13.0 } },
    };
    Camera m_look_camera{
        { .position = { 9.0, 9.0, 13.0 } },
    };
    std::optional<PlayerClientCaptureOptions> m_capture;
    std::optional<PlayerClientBenchmarkOptions> m_benchmark;
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
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> m_height_tile_interest;
    std::deque<shared::HeightTileKey> m_pending_preview_removals;
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> m_pending_preview_removal_set;
    PlayerPreviewMeshQueue m_pending_preview_meshes;
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> m_preview_mesh_jobs;
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> m_preview_mesh_dirty_jobs;
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> m_visible_preview_meshes;
    PlayerPreviewMeshCoverage m_preview_mesh_coverage;
    std::unordered_map<shared::HeightTileKey, HeightTileHandle, PlayerHeightTileKeyHash>
        m_visible_preview_mesh_tiles;
    std::unordered_map<shared::HeightTileKey, shared::HeightTileSurfaceMesh, PlayerHeightTileKeyHash>
        m_visible_preview_mesh_bases;
    std::unordered_map<
        shared::HeightTileKey,
        std::array<shared::HeightTileSurfaceElevationRange, 4>,
        PlayerHeightTileKeyHash
    > m_visible_preview_mesh_elevation_ranges;
    std::unordered_map<shared::HeightTileKey, PreviewMeshSeamBridgeSet, PlayerHeightTileKeyHash>
        m_visible_preview_mesh_seam_bridges;
    PlayerPreviewLod m_preview_lod;
    std::unordered_map<shared::HeightTileKey, PreviewTileElevation, PlayerHeightTileKeyHash>
        m_preview_tile_elevations;
    shared::HeightTileSurfaceProjection m_preview_mesh_projection{};
    std::unique_ptr<PreviewMeshWorkerPool> m_preview_mesh_workers;
    uint64_t m_preview_mesh_epoch = 1U;
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
