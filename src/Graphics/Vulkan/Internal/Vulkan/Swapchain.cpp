#include "Swapchain.hpp"
#include <Core/IO/Logger.hpp>
#include <Graphics/Window/Window.hpp>
#include "SwapchainSupport.hpp"
#include "Vulkan.hpp"
#include "../../Exception.hpp"
#include "../Check.hpp"
#include "../Queue.hpp"
#include "../CommandBuffer.hpp"

namespace graphics::vulkan::internal {
namespace {
    VkSwapchainCreateInfoKHR makeSwapchainCreateInfo(Vulkan& vulkan, window::Window& win) {
        SwapchainSupport const& support  = vulkan.physicalDevice().swapchainSupport();
        VkSurfaceFormatKHR surfaceFormat = support.chooseSurfaceFormat();
        VkPresentModeKHR   presentMode   = support.choosePresentMode();
        VkExtent2D         extent        = support.chooseSwapExtent(win);
        u32 imageCount = support.capabilities.minImageCount + 1;
        if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount)
            imageCount = support.capabilities.maxImageCount;

        VkSwapchainCreateInfoKHR createInfo { };
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = vulkan.surface().get();
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        createInfo.preTransform = support.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        static thread_local u32 queueFamilyIndices[] = { vulkan.queueFamilies().getIndex(QueueType::Graphics), vulkan.queueFamilies().getIndex(QueueType::Present) };
        if (queueFamilyIndices[0] != queueFamilyIndices[1]) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        core::io::info(
            "Chosen Vulkan swapchain options:\n\t" \
            "extent       {}x{}\n\t"               \
            "imageCount   {}\n\t"                  \
            "present mode {}",
            extent.width, extent.height,
            imageCount,
            (presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo"));
        return createInfo;
    }

    core::collection::DynArray<VkImage> getSwapchainImages(Vulkan& vulkan, VkSwapchainKHR swapchain) {
        return vulkan.enumerate<vkGetSwapchainImagesKHR>(swapchain);
    }

    core::collection::DynArray<VkImageView> getSwapchainImageViews(
        Vulkan& vulkan, 
        core::collection::DynArray<VkImage> const& images,
        VkSurfaceFormatKHR const& surfaceFormat
    ) {
        core::collection::DynArray<VkImageView> result(images.size());
        auto viewIt = result.begin();
        for (VkImage const& img : images) {
            VkImageViewCreateInfo createInfo { };
            createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.image = img;
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format = surfaceFormat.format;
            createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            createInfo.subresourceRange.baseMipLevel = 0;
            createInfo.subresourceRange.levelCount = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount = 1;
            *(viewIt++) = vulkan.create<vkCreateImageView>(&createInfo, nullptr);
        }
        return result;
    }
} // namespace


    Swapchain::Swapchain(Vulkan& vulkan, window::Window& win)
        : m_vulkan(vulkan)
        , m_imageAvailable(m_vulkan)
        , m_renderingDone(m_vulkan)
    {
        core::io::info("Creating Vulkan swapchain...");
        VkSwapchainCreateInfoKHR createInfo = makeSwapchainCreateInfo(m_vulkan, win);
        m_surfaceFormat.format     = createInfo.imageFormat;
        m_surfaceFormat.colorSpace = createInfo.imageColorSpace;
        m_presentMode              = createInfo.presentMode;
        m_extent                   = createInfo.imageExtent;
        m_imageCount               = createInfo.minImageCount;
        m_swapchain                = m_vulkan.create<vkCreateSwapchainKHR>(&createInfo, nullptr);
        QueueMaker queueMaker { m_vulkan.device().get(), m_vulkan.queueFamilies() };
        m_graphicsQueue = queueMaker.make(internal::QueueType::Graphics);
        m_presentQueue  = queueMaker.make(internal::QueueType::Present);
        m_images        = getSwapchainImages(m_vulkan, m_swapchain);
        m_imageViews    = getSwapchainImageViews(m_vulkan, m_images, m_surfaceFormat);
    }

    Swapchain::~Swapchain() {
        if (m_swapchain != VK_NULL_HANDLE) {
            for (VkImageView view : m_imageViews)
                m_vulkan.destroy<vkDestroyImageView>(view, nullptr);
            m_vulkan.destroy<vkDestroySwapchainKHR>(m_swapchain, nullptr);
            core::io::info("Destroyed Vulkan swapchain");
        }
    }
    
    u32 Swapchain::acquireNextFrame() {
        u32 result;
        if (!m_vulkan.safeCall<vkAcquireNextImageKHR>(m_swapchain, UINT64_MAX, m_imageAvailable.get(), VK_NULL_HANDLE, &result)) {
            core::io::error("Failed to acquire next frame");
            return static_cast<u32>(-1);
        }
        core::io::trace("Acquired frame {}", result);
        return result;
    }

    void Swapchain::submit(CommandBuffer& commandBuffer) {
        m_graphicsQueue->submit(commandBuffer, m_imageAvailable, m_renderingDone);
    }

    void Swapchain::present(u32 index) {
        VkSemaphore signalSemaphores[] = { m_renderingDone.get() };
        VkPresentInfoKHR presentInfo { };
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.pNext = nullptr;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &m_swapchain;
        presentInfo.pImageIndices = &index;
        presentInfo.pResults = nullptr;

        if (!VK_CHECK(vkQueuePresentKHR(m_presentQueue->get(), &presentInfo))) {
            core::io::error("Failed to present image (index {}) to present queue", index);
            throw VulkanException { };
        }

        m_presentQueue->waitIdle();
    }
} // namespace graphics::vulkan::internal
