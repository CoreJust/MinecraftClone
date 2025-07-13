#pragma once
#include <cstdint>
#include <vector>
#include <Core/Memory/UniquePtr.hpp>
#include <Core/Common/Version.hpp>
#include <Core/Collection/DynArray.hpp>
#include "RenderPipeline.hpp"
#include "PipelineOptions.hpp"

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
        uint32_t m_frameIndex = static_cast<uint32_t>(-1);

    public:
        VulkanManager() noexcept = default;
        VulkanManager(
            void* window,
            void* surfaceCreator,
            char const* const appName,
            core::common::Version const& appVersion,
            char const** windowRequiredExtensions,
            uint32_t windowRequiredExtensionsCount);
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
