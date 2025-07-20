#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Graphics/Vulkan/Pipeline/PipelineOptions.hpp>
#include "../Wrapper/Handles.hpp"
#include "../Framebuffers.hpp"
#include "PipelineStage.hpp"
#include "RenderPass.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;
    class Swapchain;
    class CommandBuffer;

    class Pipeline final {
        RenderPass m_pass;
        Framebuffers m_framebuffers;
        VkPipelineLayout m_layout = VK_NULL_HANDLE;
        VkPipeline m_pipeline = VK_NULL_HANDLE;
        u32 m_pushConstantsSizes[static_cast<usize>(PipelineStage::PipelineStagesCount)];
        Vulkan& m_vulkan;

    public:
        Pipeline(Vulkan& vulkan, pipeline::PipelineOptions const& options);
        ~Pipeline();

        void beginRenderPass(CommandBuffer& commandBuffer, u32 index);
        void endRenderPass(CommandBuffer& frame);

        PURE u32 getPushConstantsSize(PipelineStage stage) const noexcept { return m_pushConstantsSizes[static_cast<usize>(stage)]; }

        PURE VkPipeline get() const noexcept { return m_pipeline; }
        PURE VkPipelineLayout layout() const noexcept { return m_layout; }
        PURE RenderPass const& renderPass() const noexcept { return m_pass; }
    };
} // namespace graphics::vulkan::internal
