#include "RenderEngine.hpp"
#include <cmath>
#include <Core/Common/Time.hpp>
#include <Core/Common/Random.hpp>
#include <Core/IO/Logger.hpp>
#include <Core/Math/Mat.hpp>

namespace graphics {
    RenderEngine::RenderEngine(char const* const name, core::Version const& appVersion) 
        : m_window(name, {{ 800, 600 }})
        , m_vulkanManager(core::makeUP<vulkan::VulkanManager>(m_window, appVersion))
        , m_voxelPipeline(m_vulkanManager->createPipeline<renderer::pipelines::VoxelPipeline>())
        , m_voxelVertices(m_vulkanManager->createVertexBuffer<renderer::pipelines::VoxelVertex>(45))
    {
        m_window.onResize(true, [this](core::Vec2<int>) { m_vulkanManager->requestSwapchainRecreation(); });

        auto vertices = m_voxelVertices.vertices();
        core::Random rand;
        for (auto& vertex : vertices.arrayView()) {
            vertex.position = { rand.randf(-1.f, 1.f), rand.randf(-1.f, 1.f) };
            vertex.color    = { rand.randNormalized(), rand.randNormalized(), rand.randNormalized() };
        }
    }

    void RenderEngine::run() {
        static core::Mat4f MVP = {
            1.f, 0.f, 0.f, 0.f,
            0.f, 1.f, 0.f, 0.f,
            0.f, 0.f, 1.f, 0.f,
            0.f, 0.f, 0.f, 1.f,
        };
        const static core::Time start = core::Time::now();
        while (m_window.nextFrame()) {
            try {
                if (!m_vulkanManager->beginFrame())
                    continue;
                m_vulkanManager->beginRendering(m_voxelPipeline);
                MVP[0][3] = sinf(static_cast<float>((core::Time::now() - start).asSeconds()));
                MVP[1][3] = cosf(static_cast<float>((core::Time::now() - start).asSeconds()));
                m_vulkanManager->pushConstants(vulkan::internal::ShaderStageBit::Vertex, core::RawMemory::ofObject(MVP));
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
