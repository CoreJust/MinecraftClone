#include "VulkanManager.hpp"
#include <cstdlib>
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include <Graphics/Window/Window.hpp>
#include "Internal/Vulkan/ErrorCallbacks.hpp"
#include "Internal/Vulkan/Vulkan.hpp"
#include "Internal/Pipeline.hpp"
#include "Internal/CommandPool.hpp"
#include "Internal/CommandBuffer.hpp"

namespace graphics::vulkan {
    VulkanManager::VulkanManager(window::Window& win, core::Version const& appVersion) 
        : m_vulkan(core::makeUP<internal::Vulkan>(win, appVersion))
        , m_pWindow(win) {
        internal::setOutOfDateKHRCallback([this] {
            m_requiresSwapchainRecreation = true;
            return true;
        });
        core::info("Created Vulkan manager");
    }

    VulkanManager::~VulkanManager() {
        m_vulkan->device().waitIdle();
    }

    bool VulkanManager::startFrame() {
        ASSERT(m_frame == nullptr, "Frame was not ended before starting the next one");
        m_frame = m_vulkan->swapchain().acquireNextFrame();
        if (m_requiresSwapchainRecreation) {
            m_frame = nullptr;
            onSwapchainRecreationRequest();
            return false;
        }
        if (m_frame == nullptr)
            return false;
        m_frame->commandBuffer().begin();
        return true;
    }

    void VulkanManager::endFrame() {
        ASSERT(m_frame != nullptr, "Frame was not started");
        m_frame->commandBuffer().end();
        m_vulkan->swapchain().submit();
        m_vulkan->swapchain().present();
        m_frame = nullptr;
        if (m_requiresSwapchainRecreation)
            onSwapchainRecreationRequest();
    }

    RenderPipeline VulkanManager::createPipeline(PipelineOptions options) {
        auto pipeline = core::makeUP<internal::Pipeline>(*m_vulkan, options);
        usize index = m_pipelines.size();
        m_pipelines.push_back(PipelineNote {
            .options  = std::move(options),
            .pipeline = std::move(pipeline),
        });
        return RenderPipeline { index };
    }

    void VulkanManager::beginRendering(RenderPipeline& pipeline) {
        ASSERT(m_frame != nullptr, "Frame was not started; cannot begin rendering");
        auto& impl = m_pipelines[pipeline.getIndex()].pipeline;
        impl->beginRenderPass(m_frame->commandBuffer(), m_vulkan->swapchain().swapchainIndex());
    }

    void VulkanManager::endRendering(RenderPipeline& pipeline) {
        ASSERT(m_frame != nullptr, "Frame was not started; cannot end rendering");
        auto& impl = m_pipelines[pipeline.getIndex()].pipeline;
        impl->endRenderPass(m_frame->commandBuffer());
    }
        
    void VulkanManager::onSwapchainRecreationRequest() {
        m_requiresSwapchainRecreation = false;
        m_vulkan->device().waitIdle();

        core::Vec2u32 newWindowSize = m_pWindow.pixelSize();
        if (newWindowSize[0] == 0 && newWindowSize[1] == 0)
            return; // Minimized
        m_vulkan->recreateSwapchain(newWindowSize);
    }

    void VulkanManager::createPipelines() {

    }
} // namespace graphics::vulkan

