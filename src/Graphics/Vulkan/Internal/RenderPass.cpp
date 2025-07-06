#include "RenderPass.hpp"
#include <Core/IO/Logger.hpp>
#include "Check.hpp"
#include "Swapchain.hpp"
#include "Device.hpp"
#include "../Exception.hpp"

namespace graphics::vulkan::internal {
    RenderPass::RenderPass(Device& device, Swapchain& swapchain) 
        : m_device(device.get()) {
        VkAttachmentDescription colorAttachment { };
        colorAttachment.format = swapchain.surfaceFormat().format;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorAttachmentRef { };
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass { };
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;

        VkRenderPassCreateInfo renderPassInfo { };
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;

        if (!VK_CHECK(vkCreateRenderPass(device.get(), &renderPassInfo, nullptr, &m_pass))) {
            core::io::error("Failed to create render pass");
            throw VulkanException { };
        }
    }

    RenderPass::~RenderPass() {
        if (m_pass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_pass, nullptr);
            m_pass = VK_NULL_HANDLE;
        }
    }
} // namespace graphics::vulkan::internal
