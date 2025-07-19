#include "Framebuffers.hpp"
#include <vulkan/vulkan.h>
#include <Core/IO/Logger.hpp>
#include "Check.hpp"
#include "Vulkan/Vulkan.hpp"
#include "Vulkan/SwapchainFormat.hpp"
#include "Pipeline/RenderPass.hpp"
#include "../Exception.hpp"

namespace graphics::vulkan::internal {
    Framebuffers::Framebuffers(Vulkan& vulkan, RenderPass& renderPass) 
        : m_buffers(vulkan.swapchain().imageViews().size(), VK_NULL_HANDLE)
        , m_vulkan(vulkan) {
        auto const& imageViews = m_vulkan.swapchain().imageViews();
        for (usize i = 0; i < imageViews.size(); i++) {
            VkImageView attachments[] = { imageViews[i] };
            VkFramebufferCreateInfo framebufferInfo { };
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = renderPass.get();
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = attachments;
            framebufferInfo.width  = m_vulkan.swapchain().format().extent.width;
            framebufferInfo.height = m_vulkan.swapchain().format().extent.height;
            framebufferInfo.layers = 1;

            m_buffers[i] = m_vulkan.create<vkCreateFramebuffer>(&framebufferInfo, nullptr);
        }
        core::info("Created Vulkan framebuffers");
    }

    Framebuffers::~Framebuffers() {
        for (auto& framebuffer : m_buffers)
            m_vulkan.destroy<vkDestroyFramebuffer>(framebuffer, nullptr);
        core::info("Destroyed Vulkan framebuffers");
    }
} // namespace graphics::vulkan::internal
