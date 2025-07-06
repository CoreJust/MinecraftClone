#include "Swapchain.hpp"
#include <Core/IO/Logger.hpp>
#include "Check.hpp"
#include "SwapchainSupport.hpp"
#include "PhysicalDevice.hpp"
#include "../Exception.hpp"

namespace graphics::vulkan::internal {
namespace {
    void makeSwapchainCreateInfo(VkSwapchainCreateInfoKHR& createInfo, PhysicalDevice const& device, VkSurfaceKHR surface, void* window) {
        SwapchainSupport const& support  = device.swapchainSupport();
        VkSurfaceFormatKHR surfaceFormat = support.chooseSurfaceFormat();
        VkPresentModeKHR   presentMode   = support.choosePresentMode();
        VkExtent2D         extent        = support.chooseSwapExtent(window);
        uint32_t imageCount = support.capabilities.minImageCount + 1;
        if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount)
            imageCount = support.capabilities.maxImageCount;

        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface;
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

        static thread_local uint32_t queueFamilyIndices[] = { device.queueFamilies().getIndex(QueueType::Graphics), device.queueFamilies().getIndex(QueueType::Present) };
        if (queueFamilyIndices[0] != queueFamilyIndices[1]) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            createInfo.queueFamilyIndexCount = 0;
            createInfo.pQueueFamilyIndices = nullptr;
        }

        core::io::info(
            "Chosen Vulkan swapchain options:\n\t" \
            "extent     {}x{}\n\t"                 \
            "imageCount {}",
            extent.width, extent.height,
            imageCount);
    }

    core::collection::DynArray<VkImage> getSwapchainImages(VkDevice device, VkSwapchainKHR swapchain) {
        core::collection::DynArray<VkImage> result;
        uint32_t imageCount;
        if (!VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr))) {
            core::io::fatal("Failed to get swapchain images count");
            throw VulkanException { };
        }
        result.resize(static_cast<size_t>(imageCount));
        if (!VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, result.data()))) {
            core::io::fatal("Failed to get swapchain images");
            throw VulkanException { };
        }
        return result;
    }

    core::collection::DynArray<VkImageView> getSwapchainImageViews(
        VkDevice device, 
        core::collection::DynArray<VkImage> const& images,
        VkSurfaceFormatKHR const& surfaceFormat
    ) {
        core::collection::DynArray<VkImageView> result(images.size());
        auto viewIt = result.begin();
        for (VkImage const& img : images) {
            VkImageViewCreateInfo createInfo{};
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
            VkImageView& view = *(viewIt++);
            if (!VK_CHECK(vkCreateImageView(device, &createInfo, nullptr, &view))) {
                core::io::fatal("Failed to create image views for swapchain");
                throw VulkanException { };
            }
        }
        return result;
    }
} // namespace


    Swapchain::Swapchain(PhysicalDevice const& physicalDevice, VkDevice device, VkSurfaceKHR surface, void* window)
        : m_device(device)
    {
        core::io::info("Creating Vulkan swapchain...");
        VkSwapchainCreateInfoKHR createInfo { };
        makeSwapchainCreateInfo(createInfo, physicalDevice, surface, window);
        m_surfaceFormat.format     = createInfo.imageFormat;
        m_surfaceFormat.colorSpace = createInfo.imageColorSpace;
        m_presentMode              = createInfo.presentMode;
        m_extent                   = createInfo.imageExtent;
        m_imageCount               = createInfo.minImageCount;
        if (!VK_CHECK(vkCreateSwapchainKHR(device, &createInfo, nullptr, &m_swapchain))) {
            core::io::fatal("Failed to create swapchain");
            throw VulkanException { };
        }
        m_images = getSwapchainImages(device, m_swapchain);
        m_imageViews = getSwapchainImageViews(device, m_images, m_surfaceFormat);
    }

    Swapchain::~Swapchain() {
        if (m_swapchain != VK_NULL_HANDLE) {
            for (VkImageView view : m_imageViews)
                vkDestroyImageView(m_device, view, nullptr);
            vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
            m_swapchain = VK_NULL_HANDLE;
            core::io::info("Destroyed Vulkan swapchain");
        }
    }
} // namespace graphics::vulkan::internal
