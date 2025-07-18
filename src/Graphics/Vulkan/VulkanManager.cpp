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
    VulkanManager::VulkanManager(StateSnapshot snapshot, window::Window& win)
        : m_appVersion(snapshot.appVersion)
        , m_vulkan(core::makeUP<internal::Vulkan>(win, snapshot.appVersion))
        , m_pWindow(win) {
        m_pipelines.reserve(snapshot.pipelineOptions.size());
        for (auto& options : snapshot.pipelineOptions) {
            auto pipeline = core::makeUP<internal::Pipeline>(*m_vulkan, options);
            m_pipelines.push_back(PipelineNote {
                .options  = core::move(options),
                .pipeline = core::move(pipeline),
            });
        }
        core::info("Recreated Vulkan manager");
    }

    VulkanManager::VulkanManager(window::Window& win, core::Version const& appVersion)
        : m_appVersion(appVersion)
        , m_vulkan(core::makeUP<internal::Vulkan>(win, appVersion))
        , m_pWindow(win) {
        internal::setOutOfDateKHRCallback([this] {
            core::trace("Requires swapchain recreation");
            m_requiresSwapchainRecreation = true;
            return true;
        });
        internal::setOutOfDateKHRCallback([this] {
            core::trace("Prefers swapchain recreation: suboptimal swapchain");
            m_wantsSwapchainRecreation = true;
            return true;
        });
        internal::setDeviceLostCallback([] -> bool {
            core::warn("Device lost");
            throw DeviceLostException { };
        });
        core::info("Created Vulkan manager");
    }

    VulkanManager::~VulkanManager() {
        m_vulkan->device().waitIdle();
    }

    VulkanManager::StateSnapshot VulkanManager::makeSnapshot() {
        std::vector<PipelineOptions> pipelineOptions;
        pipelineOptions.reserve(m_pipelines.size());
        for (auto& [options, _] : m_pipelines)
            pipelineOptions.push_back(core::move(options));
        return {
            .appVersion = m_appVersion,
            .pipelineOptions = core::move(pipelineOptions),
        };
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
        if (m_requiresSwapchainRecreation || m_wantsSwapchainRecreation)
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
        m_wantsSwapchainRecreation = false;

        core::Vec2u32 newWindowSize = m_pWindow.pixelSize();
        if (newWindowSize[0] == 0 && newWindowSize[1] == 0)
            return; // Minimized
        m_vulkan->device().waitIdle();
        m_vulkan->recreateSwapchain(newWindowSize);
        createPipelines();
    }

    void VulkanManager::createPipelines() {
        for (auto& [options, pipeline] : m_pipelines) {
            pipeline = core::makeUP<internal::Pipeline>(*m_vulkan, options);
        }
    }
} // namespace graphics::vulkan

