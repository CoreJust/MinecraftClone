#include "RenderEngine.hpp"

namespace graphics {
    RenderEngine::RenderEngine(char const* const name, core::common::Version const& appVersion) 
        : m_window(name, 0, 0)
        , m_vulkanManager(m_window.createVulkanManager(appVersion))
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
