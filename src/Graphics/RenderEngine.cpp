#include "RenderEngine.hpp"
#include "Vulkan/Exception.hpp"

namespace graphics {
    RenderEngine::RenderEngine(char const* const name, core::Version const& appVersion) 
        : m_window(name)
        , m_vulkanManager(core::makeUP<vulkan::VulkanManager>(m_window, appVersion))
        , m_pipeline(m_vulkanManager->createPipeline({ "basic.vert", "basic.frag" }))
    { }

    void RenderEngine::run() {
        while (m_window.nextFrame()) {
            if (!m_vulkanManager->startFrame())
                continue;
            m_vulkanManager->beginRendering(m_pipeline);
            m_vulkanManager->endRendering(m_pipeline);
            m_vulkanManager->endFrame();
        }
    }
} // namespace graphics
