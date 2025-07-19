#include "RenderEngine.hpp"
#include <Core/Common/Random.hpp>

namespace graphics {
    RenderEngine::RenderEngine(char const* const name, core::Version const& appVersion) 
        : m_window(name, {{ 800, 600 }})
        , m_vulkanManager(core::makeUP<vulkan::VulkanManager>(m_window, appVersion))
        , m_voxelPipeline(m_vulkanManager->createPipeline<renderer::pipelines::VoxelPipeline>())
        , m_voxelVertices(m_vulkanManager->createVertexBuffer<renderer::pipelines::VoxelVertex>(21))
    {
        m_window.onResize(true, [this](core::Vec2<int>) { m_vulkanManager->requestSwapchainRecreation(); });

        auto vertices = m_voxelVertices.vertices();
        core::Random rand;
        for (auto& vertex : vertices.arrayView()) {
            vertex.position = { rand.randf(-1.f, 1.f), rand.randf(-1.f, 1.f) };
            vertex.color = { rand.randNormalized(), rand.randNormalized(), rand.randNormalized() };
        }
    }

    void RenderEngine::run() {
        while (m_window.nextFrame()) {
            try {
                if (!m_vulkanManager->beginFrame())
                    continue;
                m_vulkanManager->beginRendering(m_voxelPipeline);
                m_vulkanManager->drawVertices(m_voxelVertices);
                m_vulkanManager->endRendering(m_voxelPipeline);
                m_vulkanManager->endFrame();
            } catch(vulkan::DeviceLostException) {
                auto snapshot = m_vulkanManager->makeSnapshot();
                m_vulkanManager.reset();
                m_vulkanManager = core::makeUP<vulkan::VulkanManager>(core::move(snapshot), m_window);
            }
        }
    }
} // namespace graphics
