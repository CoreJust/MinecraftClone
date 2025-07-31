#include "RenderEngine.hpp"
#include <cmath>
#include <Core/Common/Time.hpp>
#include <Core/Common/Random.hpp>
#include <Core/IO/Logger.hpp>
#include <Core/Math/Transform.hpp>

namespace graphics {
    RenderEngine::RenderEngine(char const* const name, core::Version const& appVersion, engine::Camera& camera)
        : m_window(name, {{ 800, 600 }})
        , m_vulkanManager(core::makeUP<vulkan::VulkanManager>(m_window, appVersion))
        , m_pCamera(camera)
        , m_voxelPipeline(m_vulkanManager->createPipeline<renderer::pipelines::VoxelPipeline>())
        , m_voxelVertices(m_vulkanManager->createVertices<renderer::pipelines::VoxelVertex, u16>(15, 15 * 9))
    {
        m_pCamera.setAspectRatio(m_window.aspectRatio());
        m_window.onResize(true, [this](core::Vec2<int>) { 
            m_vulkanManager->requestSwapchainRecreation();
            m_pCamera.setAspectRatio(m_window.aspectRatio());
        });

        m_pCamera.move({ 0.0, 0.0, -4.0 });

        auto vertices = m_voxelVertices.mapVertices();
        core::Random rand;
        for (auto& vertex : vertices.arrayView()) {
            vertex.position = { rand.randf(-3.f, 3.f), rand.randf(-3.f, 3.f) };
            vertex.color    = { rand.randNormalized(), rand.randNormalized(), rand.randNormalized() };
            if (vertex.position[0] < -0.5f) vertex.position[0] = (vertex.position[0] + 0.5f) / 5.f - 0.5f;
            if (vertex.position[0] >  0.5f) vertex.position[0] = (vertex.position[0] - 0.5f) / 5.f + 0.5f;
            if (vertex.position[1] < -0.5f) vertex.position[1] = (vertex.position[1] + 0.5f) / 5.f - 0.5f;
            if (vertex.position[1] >  0.5f) vertex.position[1] = (vertex.position[1] - 0.5f) / 5.f + 0.5f;
        }

        auto indices = m_voxelVertices.mapIndices();
        for (auto& index : indices.arrayView())
            index = static_cast<u16>(rand.randi(0, 15));
    }
    
    void RenderEngine::run() {
        const static core::Time start = core::Time::now();
        auto p = [&](double a, double b, float c, float d) {
            return std::sinf(static_cast<float>(((core::Time::now() - start).asSeconds() - a) * b * 0.4)) * c + d;
        };
        while (m_window.nextFrame()) {
            try {
                if (!m_vulkanManager->beginFrame())
                    continue;
                m_vulkanManager->beginRendering(m_voxelPipeline);
                core::Transform3f mvp = m_pCamera.projectionView().to<float>()
                    * core::Transform3f::translation({ p(0.0, 1.0, 1.0, 0.0), p(1.0, 0.6, 1.0, 0.0), p(0.15, 0.35, 1.0, 0.0) })
                    * core::Transform3f::rotation   ({ p(0.0, 1.0, 1.0, 0.0), p(1.0, 0.6, 1.0, 0.0), p(0.15, 0.35, 1.0, 0.0) })
                    * core::Transform3f::scale      ({ p(0.0, 1.0, 1.0, 0.0), p(1.0, 0.6, 1.0, 0.0), p(0.15, 0.35, 1.0, 0.0) });
                m_vulkanManager->pushConstants(vulkan::internal::ShaderStageBit::Vertex, core::RawMemory::ofObject(mvp));
                m_vulkanManager->drawVertices(m_voxelVertices);
                m_vulkanManager->endRendering(m_voxelPipeline);
                m_vulkanManager->endFrame();
            } catch (vulkan::DeviceLostException) {
                auto snapshot = m_vulkanManager->makeSnapshot();
                m_vulkanManager.reset();
                m_vulkanManager = core::makeUP<vulkan::VulkanManager>(core::move(snapshot), m_window);
            }
        }
    }
} // namespace graphics
