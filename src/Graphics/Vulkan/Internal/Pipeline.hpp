#pragma once
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>
#include "RenderPass.hpp"
#include "Framebuffers.hpp"

namespace graphics::vulkan::internal {
    class Device;
    class Swapchain;

    class Pipeline final {
        RenderPass m_pass;
        Framebuffers m_framebuffers;
        VkPipelineLayout m_layout = VK_NULL_HANDLE;
        VkPipeline m_pipeline = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE; // Device used to create the pipeline

    public:
        Pipeline(Device& device, Swapchain& swapchain, char const* const vertexShaderPath, char const* const fragmentShaderPath);
        ~Pipeline();

        PURE VkPipeline get() const noexcept { return m_pipeline; }
        PURE VkPipelineLayout layout() const noexcept { return m_layout; }
        PURE RenderPass const& renderPass() const noexcept { return m_pass; }
    };
} // namespace graphics::vulkan::internal
