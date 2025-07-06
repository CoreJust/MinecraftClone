#pragma once
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>
#include <Core/Collection/DynArray.hpp>

namespace graphics::vulkan::internal {
    struct SwapchainSupport;
    class PhysicalDevice;

    class Swapchain final {
        core::collection::DynArray<VkImage> m_images;
        core::collection::DynArray<VkImageView> m_imageViews;
        VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE; // Device used for swapchain creation.
        VkSurfaceFormatKHR m_surfaceFormat;
        VkPresentModeKHR m_presentMode;
        VkExtent2D m_extent;
        uint32_t m_imageCount;

    public:
        Swapchain(PhysicalDevice const& physicalDevice, VkDevice device, VkSurfaceKHR surface, void* window);
        ~Swapchain();

        PURE VkSwapchainKHR get() const noexcept { return m_swapchain; }
        PURE core::collection::DynArray<VkImageView> const& imageViews() const noexcept { return m_imageViews; }
        PURE VkSurfaceFormatKHR const& surfaceFormat() const noexcept { return m_surfaceFormat; }
        PURE VkExtent2D const& extent() const noexcept { return m_extent; }
        PURE uint32_t imageCount() const noexcept { return m_imageCount; }
    };
} // namespace graphics::vulkan::internal
