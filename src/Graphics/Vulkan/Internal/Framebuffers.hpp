#pragma once
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>
#include <Core/Collection/DynArray.hpp>

namespace graphics::vulkan::internal {
    class Device;
    class Swapchain;
    class RenderPass;

    class Framebuffers final {
        core::collection::DynArray<VkFramebuffer> m_buffers;
        VkDevice m_device = VK_NULL_HANDLE; // Device used for framebuffers creation

    public:
        Framebuffers(Device& device, Swapchain& swapchain, RenderPass& renderPass);
        ~Framebuffers();

        PURE core::collection::DynArray<VkFramebuffer> const& get() const noexcept { return m_buffers; }
    };
} // namespace graphics::vulkan::internal
