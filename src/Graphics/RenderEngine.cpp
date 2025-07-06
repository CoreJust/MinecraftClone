#include "RenderEngine.hpp"

namespace graphics {
    RenderEngine::RenderEngine(char const* const name) 
        : m_window(name, 1920, 1080)
        , m_vulkan(m_window.createVulkan())
        , m_pipeline(m_vulkan->createPipeline("basic.vert", "basic.frag"))
    { }

    void RenderEngine::run() {
        while (m_window.nextFrame());
    }
} // namespace graphics
