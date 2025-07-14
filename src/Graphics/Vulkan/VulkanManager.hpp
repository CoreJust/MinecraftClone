#pragma once
#include <vector>
#include <Core/Common/Int.hpp>
#include <Core/Common/Version.hpp>
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
            core::memory::UniquePtr<internal::Pipeline> pipeline;
        };

        core::memory::UniquePtr<internal::Vulkan> m_vulkan;
        core::memory::UniquePtr<internal::CommandPool> m_commandPool;
        core::collection::DynArray<internal::CommandBuffer> m_commandBuffers;
        std::vector<PipelineNote> m_pipelines;
        u32 m_frameIndex = static_cast<u32>(-1);

    public:
        VulkanManager() noexcept = default;
        VulkanManager(window::Window& win, core::common::Version const& appVersion);
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
        void createPipelines();
    };
} // namespace graphics::vulkan
