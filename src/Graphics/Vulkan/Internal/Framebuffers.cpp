#include "Framebuffers.hpp"
#include <Core/IO/Logger.hpp>
#include "Check.hpp"
#include "Device.hpp"
#include "Swapchain.hpp"
#include "RenderPass.hpp"
#include "../Exception.hpp"

namespace graphics::vulkan::internal {
    Framebuffers::Framebuffers(Device& device, Swapchain& swapchain, RenderPass& renderPass) 
        : m_buffers(swapchain.imageViews().size(), VK_NULL_HANDLE)
        , m_device(device.get()) {
        auto const& imageViews = swapchain.imageViews();
        for (size_t i = 0; i < imageViews.size(); i++) {
            VkImageView attachments[] = { imageViews[i] };
            VkFramebufferCreateInfo framebufferInfo { };
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = renderPass.get();
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = attachments;
            framebufferInfo.width = swapchain.extent().width;
            framebufferInfo.height = swapchain.extent().height;
            framebufferInfo.layers = 1;

            if (!VK_CHECK(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_buffers[i]))) {
                core::io::error("Failed to create framebuffers");
                throw VulkanException { };
            }
        }
        core::io::info("Created Vulkan framebuffers");
    }

    Framebuffers::~Framebuffers() {
        for (auto& framebuffer : m_buffers) {
            if (framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(m_device, framebuffer, nullptr);
                framebuffer = VK_NULL_HANDLE;
            }
        }
        core::io::info("Destroyed Vulkan framebuffers");
    }
} // namespace graphics::vulkan::internal
