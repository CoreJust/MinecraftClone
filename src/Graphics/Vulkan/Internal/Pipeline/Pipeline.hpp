#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Container/DynArray.hpp>
#include <Graphics/Vulkan/Pipeline/PipelineOptions.hpp>
#include "../Wrapper/Handles.hpp"
#include "../Framebuffers.hpp"
#include "ShaderStageBit.hpp"
#include "DescriptorSet.hpp"
#include "RenderPass.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;
    class Swapchain;
    class CommandBuffer;

    class Pipeline final {
        RenderPass                    m_pass;
        Framebuffers                  m_framebuffers;
        core::DynArray<DescriptorSet> m_descriptors;
        VkPipelineLayout              m_layout   = VK_NULL_HANDLE;
        VkPipeline                    m_pipeline = VK_NULL_HANDLE;
        // TODO: refactor
        u32                           m_pushConstantsSizes[2];
        Vulkan&                       m_vulkan;

    public:
        Pipeline(Vulkan& vulkan, pipeline::PipelineOptions const& options);
        ~Pipeline();

        void beginRenderPass(CommandBuffer& commandBuffer, u32 index);
        void endRenderPass(CommandBuffer& frame);

        PURE u32 getPushConstantsSize(ShaderStageBit stage) const noexcept { return m_pushConstantsSizes[stage == ShaderStageBit::Fragment]; }

        PURE VkPipeline get() const noexcept { return m_pipeline; }
        PURE VkPipelineLayout layout() const noexcept { return m_layout; }
        PURE RenderPass const& renderPass() const noexcept { return m_pass; }
    };
} // namespace graphics::vulkan::internal
