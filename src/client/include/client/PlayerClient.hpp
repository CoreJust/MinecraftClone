#pragma once

#include "Camera.hpp"
#include "GameClient.hpp"
#include "render/InstalledShaderAssets.hpp"
#include "render/VulkanRenderer.hpp"

#include <shared/ProjectInfo.hpp>

#include <core/platform/glfw/GlfwWindow.hpp>

#include <vector>

namespace client {

class PlayerClient final : public GameClient {
public:
    explicit PlayerClient(shared::WorldMode const mode = shared::WorldMode::Flat);
    ~PlayerClient();
private:
    shared::Direction input() override;
    void render() override;
private:
    void beginContinuousLook() noexcept;
    Camera m_camera{
        { .position = { 9.0, 9.0, 13.0 } },
    };
    core::platform::glfw::GlfwWindow m_window;
    InstalledShaderAssets m_shader_assets;
    VulkanRenderer m_renderer;
    std::vector<PlayerRenderData> m_render_data;
    DebugHudToggleLatch m_debug_hud_toggle;
    double m_last_cursor_x = 0.0;
    double m_last_cursor_y = 0.0;
    bool m_has_cursor_position = false;
    bool m_was_reload_pressed = false;
};

} // namespace client
