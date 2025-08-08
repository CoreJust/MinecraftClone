#include "RenderEngine.hpp"
#include <Core/Common/Random.hpp>
#include <Core/IO/Logger.hpp>
#include <Core/Math/Transform.hpp>
#include "Window/Keyboard.hpp"

namespace graphics {
    RenderEngine::RenderEngine(char const* const name, core::Version const& appVersion, engine::Camera& camera)
        : m_window(name, {{ 800, 600 }})
        , m_vulkanManager(core::makeUP<vulkan::VulkanManager>(m_window, appVersion))
        , m_pCamera(camera)
        , m_voxelPipeline(m_vulkanManager->createPipeline<renderer::pipelines::VoxelPipeline>())
        , m_voxelVertices(m_vulkanManager->createVertices<renderer::pipelines::VoxelVertex, u16>(21, 21 * 6))
    {
        m_pCamera.setAspectRatio(m_window.aspectRatio());
        m_window.enableCursor(false);
        m_window.onResize(true, [this](core::Vec2<int>) { 
            m_vulkanManager->requestSwapchainRecreation();
            m_pCamera.setAspectRatio(m_window.aspectRatio());
        });

        m_pCamera.move({ 0.0, 0.0, -4.0 });

        auto vertices = m_voxelVertices.mapVertices();
        core::Random rand;
        for (auto& vertex : vertices.arrayView()) {
            vertex.position  = { rand.randf(-3.f, 3.f), rand.randf(-3.f, 5.f) , rand.randf(-3.f, 3.f) };
            vertex.texCoords = { rand.randNormalized(), rand.randNormalized() };
        }

        auto indices = m_voxelVertices.mapIndices();
        for (auto& index : indices.arrayView())
            index = static_cast<u16>(rand.randi(0, 15));
    }
    
    void RenderEngine::run() {
        while (m_window.nextFrame()) {
            try {
                m_window.enableCursor(window::isKeyPressed(window::Key::LeftAlt) || window::isKeyPressed(window::Key::RightAlt));
                if (!m_vulkanManager->beginFrame())
                    continue;
                m_vulkanManager->beginRendering(m_voxelPipeline);
                core::Transform3f mvp = m_pCamera.projectionView().to<float>();
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
