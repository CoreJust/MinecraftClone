#pragma once

#include "Camera.hpp"
#include "GameClient.hpp"
#include "render/InstalledShaderAssets.hpp"
#include "render/VulkanRenderer.hpp"

#include <shared/ProjectInfo.hpp>

#include <core/platform/glfw/GlfwWindow.hpp>

#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

namespace client {

struct PlayerClientCaptureOptions final {
    std::filesystem::path image_path;
    uint32_t minimum_height_tile_meshes = 1'024U;
};

struct PlayerHeightTileKeyHash final {
    [[nodiscard]] size_t operator()(shared::HeightTileKey key) const noexcept;
};

class PlayerClient final : public GameClient {
public:
    explicit PlayerClient(
        shared::WorldMode mode = shared::WorldMode::Flat,
        std::optional<PlayerClientCaptureOptions> capture = std::nullopt
    );
    ~PlayerClient();
private:
    class PreviewMeshWorkerPool;
    shared::Direction input() override;
    void render() override;
private:
    void beginContinuousLook() noexcept;
    void updateFlightControlToggles();
    void refreshHeightTileInterest(shared::Player const& player);
    void queuePreviewMesh(shared::HeightTileKey key);
    void processPendingPreviewMeshes(uint32_t maximum_meshes);
    Camera m_camera{
        { .position = { 9.0, 9.0, 13.0 } },
    };
    std::optional<PlayerClientCaptureOptions> m_capture;
    core::platform::glfw::GlfwWindow m_window;
    InstalledShaderAssets m_shader_assets;
    VulkanRenderer m_renderer;
    std::vector<PlayerRenderData> m_render_data;
    DebugHudToggleLatch m_debug_hud_toggle;
    DebugHudToggleLatch m_speedup_increase_latch;
    DebugHudToggleLatch m_speedup_decrease_latch;
    DebugHudToggleLatch m_acceleration_toggle_latch;
    size_t m_speedup_profile_index = 2U;
    bool m_acceleration_enabled = false;
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
    std::deque<shared::HeightTileKey> m_pending_preview_meshes;
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> m_pending_preview_mesh_set;
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> m_preview_mesh_jobs;
    std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash> m_preview_mesh_dirty_jobs;
    std::unique_ptr<PreviewMeshWorkerPool> m_preview_mesh_workers;
    bool m_capture_requested = false;
    std::optional<std::unordered_set<shared::HeightTileKey, PlayerHeightTileKeyHash>>
        m_capture_pre_rotation_interest;
    uint32_t m_capture_rotation_frames = 0U;
};

} // namespace client
