#pragma once

#include "GameClient.hpp"
#include "render/InstalledShaderAssets.hpp"
#include "render/VulkanRenderer.hpp"

#include <shared/ProjectInfo.hpp>

#include <core/platform/glfw/GlfwWindow.hpp>

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
    { }
private:
    shared::Direction input() override;
    void render() override;
private:
    core::platform::glfw::GlfwWindow m_window;
    InstalledShaderAssets m_shader_assets;
    VulkanRenderer m_renderer;
    std::vector<PlayerRenderData> m_render_data;
    DebugHudToggleLatch m_debug_hud_toggle;
    bool m_was_reload_pressed = false;
};

} // namespace client
