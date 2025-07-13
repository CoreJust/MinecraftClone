#include "VulkanManager.hpp"
#include <cstdlib>
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include "Internal/Vulkan/Vulkan.hpp"
#include "Internal/Pipeline.hpp"
#include "Internal/CommandPool.hpp"
#include "Internal/CommandBuffer.hpp"

namespace graphics::vulkan {
    VulkanManager::VulkanManager(
        void* window,
        void* surfaceCreator,
        char const* const appName,
        core::common::Version const& appVersion,
        char const** windowRequiredExtensions,
        uint32_t windowRequiredExtensionsCount) 
        : m_vulkan(core::memory::makeUP<internal::Vulkan>(window, surfaceCreator, appName, appVersion, windowRequiredExtensions, windowRequiredExtensionsCount)) {
        m_commandPool    = core::memory::makeUP<internal::CommandPool>(*m_vulkan, m_vulkan->queueFamilies().getIndex(internal::QueueType::Graphics));
        m_commandBuffers = core::collection::DynArray<internal::CommandBuffer>(m_vulkan->swapchain().imageViews().size());
        m_commandPool->allocate(m_commandBuffers);
    }

    VulkanManager::~VulkanManager() = default;

    bool VulkanManager::startFrame() {
        ASSERT(m_frameIndex == static_cast<uint32_t>(-1), "Frame was not ended before starting the next one");
        m_frameIndex = m_vulkan->swapchain().acquireNextFrame();
        if (m_frameIndex == static_cast<uint32_t>(-1))
            return false;
        m_commandBuffers[m_frameIndex].begin();
        return true;
    }

    void VulkanManager::endFrame() {
        ASSERT(m_frameIndex != static_cast<uint32_t>(-1), "Frame was not started");
        m_commandBuffers[m_frameIndex].end();
        m_vulkan->swapchain().submit(m_commandBuffers[m_frameIndex]);
        m_vulkan->swapchain().present(m_frameIndex);
        m_frameIndex = static_cast<uint32_t>(-1);
    }

    RenderPipeline VulkanManager::createPipeline(PipelineOptions options) {
        auto pipeline = core::memory::makeUP<internal::Pipeline>(*m_vulkan, options);
        size_t index = m_pipelines.size();
        m_pipelines.push_back(PipelineNote {
            .options  = std::move(options),
            .pipeline = std::move(pipeline),
        });
        return RenderPipeline { index };
    }

    void VulkanManager::beginRendering(RenderPipeline& pipeline) {
        ASSERT(m_frameIndex != static_cast<uint32_t>(-1), "Frame was not started; cannot begin rendering");
        auto& impl = m_pipelines[pipeline.getIndex()].pipeline;
        impl->beginRenderPass(m_commandBuffers[m_frameIndex], m_frameIndex);
    }

    void VulkanManager::endRendering(RenderPipeline& pipeline) {
        ASSERT(m_frameIndex != static_cast<uint32_t>(-1), "Frame was not started; cannot end rendering");
        auto& impl = m_pipelines[pipeline.getIndex()].pipeline;
        impl->endRenderPass(m_commandBuffers[m_frameIndex]);
    }

    void VulkanManager::createPipelines() {

    }
} // namespace graphics::vulkan

