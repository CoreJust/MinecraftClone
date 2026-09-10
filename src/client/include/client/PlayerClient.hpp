#pragma once

#include "GameClient.hpp"
#include "render/InstalledShaderAssets.hpp"
#include "render/VulkanRenderer.hpp"

#include <shared/ProjectInfo.hpp>

#include <core/vulkan/GlfwSurfaceProvider.hpp>
#include <core/window/Window.hpp>

namespace client {

class PlayerClient final : public GameClient {
public:
    explicit PlayerClient()
        : m_window(std::string{ shared::PROJECT_NAME })
        , m_surface_provider(m_window)
        , m_renderer(m_surface_provider, m_shader_assets)
    { }
private:
    shared::Direction input() override;
    void render() override;
private:
    core::Window m_window;
    core::vk::GlfwSurfaceProvider m_surface_provider;
    InstalledShaderAssets m_shader_assets;
    VulkanRenderer m_renderer;
    bool m_was_reload_pressed = false;
};

} // namespace client
