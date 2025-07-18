#include "RenderEngine.hpp"

namespace graphics {
    RenderEngine::RenderEngine(char const* const name, core::Version const& appVersion) 
        : m_window(name, {{ 800, 600 }})
        , m_vulkanManager(core::makeUP<vulkan::VulkanManager>(m_window, appVersion))
        , m_pipeline(m_vulkanManager->createPipeline({ "basic.vert", "basic.frag" }))
    {
        m_window.onResize(true, [this](core::Vec2<int>) { m_vulkanManager->requestSwapchainRecreation(); });
    }

    void RenderEngine::run() {
        while (m_window.nextFrame()) {
            try {
                if (!m_vulkanManager->startFrame())
                    continue;
                m_vulkanManager->beginRendering(m_pipeline);
                m_vulkanManager->endRendering(m_pipeline);
                m_vulkanManager->endFrame();
            } catch(vulkan::DeviceLostException) {
                auto snapshot = m_vulkanManager->makeSnapshot();
                m_vulkanManager.reset();
                m_vulkanManager = core::makeUP<vulkan::VulkanManager>(core::move(snapshot), m_window);
            }
        }
    }
} // namespace graphics
