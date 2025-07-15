#pragma once
#include <vector>
#include <Core/Common/Int.hpp>
#include <Core/Common/Version.hpp>
#include <Core/Math/Vec.hpp>
#include <Core/Memory/UniquePtr.hpp>
#include <Core/Collection/DynArray.hpp>
#include "RenderPipeline.hpp"
#include "PipelineOptions.hpp"

namespace graphics::window {
    class Window;
}

namespace graphics::vulkan {
    namespace internal {
        class Vulkan;
        class Queue;
        class CommandPool;
        class CommandBuffer;
        class Pipeline;
    }

    class VulkanManager final {
        struct PipelineNote final {
            PipelineOptions options;
            core::UniquePtr<internal::Pipeline> pipeline;
        };

        core::UniquePtr<internal::Vulkan> m_vulkan;
        core::UniquePtr<internal::CommandPool> m_commandPool;
        core::DynArray<internal::CommandBuffer> m_commandBuffers;
        std::vector<PipelineNote> m_pipelines;
        window::Window& m_pWindow;
        u32 m_frameIndex = static_cast<u32>(-1);
        bool m_requiresSwapchainRecreation = false;

    public:
        VulkanManager(window::Window& win, core::Version const& appVersion);
        VulkanManager(VulkanManager&&) noexcept = delete;
        VulkanManager(VulkanManager const&) noexcept = delete;
        VulkanManager& operator=(VulkanManager&&) noexcept = delete;
        VulkanManager& operator=(VulkanManager const&) noexcept = delete;
        ~VulkanManager();

        bool startFrame();
        void endFrame();

        PURE RenderPipeline createPipeline(PipelineOptions options);
        void beginRendering(RenderPipeline& pipeline);
        void endRendering(RenderPipeline& pipeline);

    private:
        void onSwapchainRecreationRequest();
        void createPipelines();
    };
} // namespace graphics::vulkan
