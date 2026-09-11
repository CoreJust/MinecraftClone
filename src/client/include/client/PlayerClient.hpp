#pragma once

#include "Camera.hpp"
#include "GameClient.hpp"
#include "render/InstalledShaderAssets.hpp"
#include "render/VulkanRenderer.hpp"

#include <shared/ProjectInfo.hpp>

#include <core/platform/glfw/GlfwWindow.hpp>

#include <optional>
#include <vector>

namespace client {

class PlayerClient final : public GameClient {
public:
    explicit PlayerClient()
        : m_window(core::platform::glfw::WindowDescriptor{
            .width = 1280U,
            .height = 720U,
            .title = std::string{ shared::PROJECT_NAME },
        })
        , m_renderer(VulkanRenderer::createPresentationContext(m_window), m_shader_assets)
    {
        beginContinuousLook();
        m_renderer.setDebugHudEnabled(true);
    }
    ~PlayerClient();
private:
    shared::Direction input() override;
    void render() override;
    void onAuthoritativeLocalPlayerPosition(shared::Player const& player) noexcept override;
private:
    void beginContinuousLook() noexcept;
    Camera m_camera{
        { .position = { 16.0, -20.0, 22.0 }, .angles = { .pitch_degrees = -35.0 } },
    };
    core::platform::glfw::GlfwWindow m_window;
    InstalledShaderAssets m_shader_assets;
    VulkanRenderer m_renderer;
    std::vector<PlayerRenderData> m_render_data;
    std::optional<shared::Player> m_local_player;
    DebugHudToggleLatch m_debug_hud_toggle;
    double m_last_cursor_x = 0.0;
    double m_last_cursor_y = 0.0;
    bool m_has_cursor_position = false;
    bool m_was_reload_pressed = false;
};

} // namespace client
