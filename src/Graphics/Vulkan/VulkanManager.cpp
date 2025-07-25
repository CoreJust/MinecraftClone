#include "VulkanManager.hpp"
#include <cstdlib>
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include <Graphics/Window/Window.hpp>
#include "Internal/Vulkan/ErrorCallbacks.hpp"
#include "Internal/Vulkan/Vulkan.hpp"
#include "Internal/Buffer/VertexBuffer.hpp"
#include "Internal/Pipeline/Pipeline.hpp"
#include "Internal/Command/CommandPool.hpp"
#include "Internal/Command/CommandBuffer.hpp"

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
        m_copyCommandPool = core::makeUP<internal::CommandPool>(
            *m_vulkan,
            internal::CommandPoolTypeBit::Transient | internal::CommandPoolTypeBit::ResetCommandBuffer, 
            m_vulkan->swapchain().graphicsQueue());
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
        m_copyCommandPool = core::makeUP<internal::CommandPool>(
            *m_vulkan,
            internal::CommandPoolTypeBit::Transient | internal::CommandPoolTypeBit::ResetCommandBuffer, 
            m_vulkan->swapchain().graphicsQueue());
        core::info("Created Vulkan manager");
    }

    VulkanManager::~VulkanManager() {
        m_vulkan->device().waitIdle();
    }

    VulkanManager::StateSnapshot VulkanManager::makeSnapshot() {
        std::vector<pipeline::PipelineOptions> pipelineOptions;
        pipelineOptions.reserve(m_pipelines.size());
        for (auto& [options, _] : m_pipelines)
            pipelineOptions.push_back(core::move(options));
        return {
            .appVersion = m_appVersion,
            .pipelineOptions = core::move(pipelineOptions),
        };
    }

    bool VulkanManager::beginFrame() {
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

    void VulkanManager::beginRendering(pipeline::RenderPipeline& pipeline) {
        ASSERT(m_frame != nullptr, "Frame was not started; cannot begin rendering");
        ASSERT(m_currentPipeline == static_cast<usize>(-1), "Already has begun a pipeline; it must be ended before beginning next one");
        m_currentPipeline = pipeline.getIndex();
        auto& impl = m_pipelines[m_currentPipeline].pipeline;
        impl->beginRenderPass(m_frame->commandBuffer(), m_vulkan->swapchain().swapchainIndex());
    }

    void VulkanManager::endRendering([[maybe_unused]] pipeline::RenderPipeline& pipeline) {
        ASSERT(m_frame != nullptr, "Frame was not started; cannot end rendering");
        ASSERT(m_currentPipeline == pipeline.getIndex(), "Cannot end pipeline: its index and the index of current pipeline differ");
        auto& impl = m_pipelines[m_currentPipeline].pipeline;
        impl->endRenderPass(m_frame->commandBuffer());
        m_currentPipeline = static_cast<usize>(-1);
    }
        
    void VulkanManager::pushConstants(internal::ShaderStageBit stage, core::RawMemory constants) {
        ASSERT(m_frame != nullptr, "Frame was not started; cannot push constants");
        ASSERT(m_currentPipeline != static_cast<usize>(-1), "No pipeline was begun; cannot push constants");
        auto& impl = m_pipelines[m_currentPipeline].pipeline;
        ASSERT(impl->getPushConstantsSize(stage) == constants.size, "PushConstantsSize in shader doesn't match the provided one; cannot push constants");
        m_frame->commandBuffer().pushConstants(impl->layout(), stage, constants);
    }

    void VulkanManager::drawVertices(pipeline::VerticesBase& vertices) {
        ASSERT(m_frame != nullptr, "Frame was not started; cannot draw vertices");
        ASSERT(m_currentPipeline != static_cast<usize>(-1), "No pipeline was begun; cannot push constants");
        m_frame->commandBuffer().drawVertices(vertices.buffer());
    }

    usize VulkanManager::createPipelineImpl(pipeline::PipelineOptions const& options) {
        auto pipeline = core::makeUP<internal::Pipeline>(*m_vulkan, options);
        usize index = m_pipelines.size();
        m_pipelines.push_back(PipelineNote {
            .options  = options,
            .pipeline = std::move(pipeline),
        });
        return index;
    }

    pipeline::VerticesBase VulkanManager::createVertexBufferImpl(usize size) {
        return { m_vulkan->make<internal::VertexBuffer>(*m_copyCommandPool, size) };
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

