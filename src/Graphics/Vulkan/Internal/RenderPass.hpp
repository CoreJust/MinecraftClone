#pragma once
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>

namespace graphics::vulkan::internal {
    class Swapchain;
    class Device;

    class RenderPass final {
        VkRenderPass m_pass = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE; // Device used to create the pass

    public:
        RenderPass(Device& device, Swapchain& swapchain);
        ~RenderPass();

        PURE VkRenderPass get() const noexcept { return m_pass; }
    };
} // namespace graphics::vulkan::internal
