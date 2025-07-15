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
        m_commandPool    = core::makeUP<internal::CommandPool>(*m_vulkan, m_vulkan->queueFamilies().getIndex(internal::QueueType::Graphics));
        m_commandBuffers = core::DynArray<internal::CommandBuffer>(m_vulkan->swapchain().imageViews().size());
        m_commandPool->allocate(m_commandBuffers);
        core::info("Created Vulkan manager");
    }

    VulkanManager::~VulkanManager() {
        m_vulkan->device().waitIdle();
    }

    bool VulkanManager::startFrame() {
        ASSERT(m_frameIndex == static_cast<u32>(-1), "Frame was not ended before starting the next one");
        m_frameIndex = m_vulkan->swapchain().acquireNextFrame();
        if (m_requiresSwapchainRecreation) {
            m_frameIndex = static_cast<u32>(-1);
            onSwapchainRecreationRequest();
            return false;
        }
        if (m_frameIndex == static_cast<u32>(-1))
            return false;
        m_commandBuffers[m_frameIndex].begin();
        return true;
    }

    void VulkanManager::endFrame() {
        ASSERT(m_frameIndex != static_cast<u32>(-1), "Frame was not started");
        m_commandBuffers[m_frameIndex].end();
        m_vulkan->swapchain().submit(m_commandBuffers[m_frameIndex]);
        m_vulkan->swapchain().present(m_frameIndex);
        m_frameIndex = static_cast<u32>(-1);
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
        ASSERT(m_frameIndex != static_cast<u32>(-1), "Frame was not started; cannot begin rendering");
        auto& impl = m_pipelines[pipeline.getIndex()].pipeline;
        impl->beginRenderPass(m_commandBuffers[m_frameIndex], m_frameIndex);
    }

    void VulkanManager::endRendering(RenderPipeline& pipeline) {
        ASSERT(m_frameIndex != static_cast<u32>(-1), "Frame was not started; cannot end rendering");
        auto& impl = m_pipelines[pipeline.getIndex()].pipeline;
        impl->endRenderPass(m_commandBuffers[m_frameIndex]);
    }
        
    void VulkanManager::onSwapchainRecreationRequest() {
        m_requiresSwapchainRecreation = false;
        m_vulkan->device().waitIdle();

        core::Vec2u32 newWindowSize = m_pWindow.pixelSize();
        if (newWindowSize[0] == 0 && newWindowSize[1] == 0) {
            core::trace("VulkanManager::onOutOfDateKHR: window got minimized");
            return;
        }
        m_vulkan->recreateSwapchain(newWindowSize);
    }

    void VulkanManager::createPipelines() {

    }
} // namespace graphics::vulkan

