#include "Swapchain.hpp"
#include <vulkan/vulkan.h>
#include <Core/Memory/Move.hpp>
#include <Core/IO/Logger.hpp>
#include <Graphics/Vulkan/Exception.hpp>
#include "SwapchainFormat.hpp"
#include "SwapchainSupport.hpp"
#include "../Command/Queue.hpp"
#include "../Command/CommandBuffer.hpp"
#include "../Check.hpp"
#include "Vulkan.hpp"

namespace graphics::vulkan::internal {
namespace {
    VkSwapchainCreateInfoKHR makeSwapchainCreateInfo(Vulkan& vulkan, core::Vec2u32 pixelSize) {
        SwapchainSupport const& support  = vulkan.physicalDevice().swapchainSupport();
        VkSurfaceFormatKHR surfaceFormat = support.chooseSurfaceFormat();
        VkPresentModeKHR   presentMode   = support.choosePresentMode();
        VkExtent2D         extent        = support.chooseSwapExtent(pixelSize);
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

        core::info(
            "Chosen Vulkan swapchain options:\n\t" \
            "extent       {}x{}\n\t"               \
            "imageCount   {}\n\t"                  \
            "present mode {}",
            extent.width, extent.height,
            imageCount,
            (presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo"));
        return createInfo;
    }

    core::DynArray<VkImage> getSwapchainImages(Vulkan& vulkan, VkSwapchainKHR swapchain) {
        return vulkan.enumerate<vkGetSwapchainImagesKHR>(swapchain);
    }

    core::DynArray<VkImageView> getSwapchainImageViews(
        Vulkan& vulkan, 
        core::DynArray<VkImage> const& images,
        VkSurfaceFormatKHR const& surfaceFormat
    ) {
        core::DynArray<VkImageView> result(images.size());
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

    Swapchain::Swapchain(Vulkan& vulkan, core::Vec2u32 pixelSize, usize framesCount)
        : m_vulkan(vulkan)
    {
        core::note("Creating Vulkan swapchain...");
        initialize(pixelSize);
        QueueMaker queueMaker { m_vulkan.device().get(), m_vulkan.queueFamilies() };
        m_graphicsQueue = queueMaker.make(internal::QueueType::Graphics);
        m_presentQueue  = queueMaker.make(internal::QueueType::Present);
        m_frames = core::DynArray<Frame>(framesCount, [&](Frame* ptr, usize) { new(ptr) Frame { vulkan, *m_graphicsQueue }; });
    }

    Swapchain::~Swapchain() {
        if (m_swapchain != VK_NULL_HANDLE) {
            for (VkImageView view : m_imageViews)
                m_vulkan.destroy<vkDestroyImageView>(view, nullptr);
            m_vulkan.destroy<vkDestroySwapchainKHR>(m_swapchain, nullptr);
            core::note("Destroyed Vulkan swapchain");
        }
    }
    
    Frame* Swapchain::acquireNextFrame() {
        Frame& frame = currentFrame();
        frame.fence().wait();

        if (!m_vulkan.safeCall<vkAcquireNextImageKHR>(m_swapchain, UINT64_MAX, frame.imageAvailable().get(), VK_NULL_HANDLE, &m_swapchainIndex)) {
            core::error("Failed to acquire next frame");
            m_swapchainIndex = static_cast<u32>(-1);
            return nullptr;
        }
        Fence*& swapchainFence = m_swapchainFences[m_swapchainIndex];
        if (swapchainFence != nullptr) 
            swapchainFence->wait();
        swapchainFence = &frame.fence();
        return &currentFrame();
    }

    void Swapchain::submit() {
        Frame& frame = currentFrame();
        frame.fence().reset();
        m_graphicsQueue->submit(frame.commandBuffer(), PipelineStageBit::ColorAttachmentOutput, &frame.imageAvailable(), &frame.renderingDone(), &frame.fence());
    }

    void Swapchain::present() {
        ASSERT(m_swapchainIndex != static_cast<u32>(-1), "No swapchain index is acquired; Cannot present");

        VkPresentInfoKHR presentInfo { };
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.pNext = nullptr;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = currentFrame().renderingDone().ptr();
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &m_swapchain;
        presentInfo.pImageIndices = &m_swapchainIndex;
        presentInfo.pResults = nullptr;

        if (!VK_CHECK(vkQueuePresentKHR(m_presentQueue->get(), &presentInfo))) {
            core::error("Failed to present image (index {}) to present queue", m_swapchainIndex);
            throw VulkanException { };
        }

        ++m_framesCounter;
    }
    
    void Swapchain::initialize(core::Vec2u32 pixelSize) {
        VkSwapchainCreateInfoKHR createInfo = makeSwapchainCreateInfo(m_vulkan, pixelSize);
        m_format = core::makeUP<SwapchainFormat>(SwapchainFormat {
            { createInfo.imageFormat, createInfo.imageColorSpace }, createInfo.presentMode, createInfo.imageExtent
        });
        m_imageCount               = createInfo.minImageCount;
        m_swapchain                = m_vulkan.create<vkCreateSwapchainKHR>(&createInfo, nullptr);
        m_images                   = getSwapchainImages(m_vulkan, m_swapchain);
        m_imageViews               = getSwapchainImageViews(m_vulkan, m_images, m_format->surfaceFormat);
        m_swapchainFences          = core::DynArray<Fence*>(m_imageCount, nullptr);
    }
} // namespace graphics::vulkan::internal
