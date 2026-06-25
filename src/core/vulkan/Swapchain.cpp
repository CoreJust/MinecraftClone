#include <core/vulkan/Swapchain.hpp>

#include <core/vulkan/Check.hpp>

#include <volk.h>

namespace core {

CORE_VK_RESOURCE_DESTROY_IMPL(RawSwapchain) {
    vkDestroySwapchainKHR(device_handle, self.m_handle, nullptr);
}

Swapchain::Swapchain(VkSwapchainKHR const swapchain, Image::Info const& info, Device const& device)
    : VulkanRaii<RawSwapchain>(swapchain, device)
{
    uint32_t image_count = 0;
    CORE_VK_ASSERT(vkGetSwapchainImagesKHR(device.handle(), m_handle, &image_count, nullptr));
    std::vector<VkImage> images(image_count);
    CORE_VK_ASSERT(vkGetSwapchainImagesKHR(device.handle(), m_handle, &image_count, images.data()));

    m_images.reserve(image_count);
    m_image_views.reserve(image_count);

    for (VkImage const image : images) {
        Image& emplaced_image = m_images.emplace_back(image, info, Allocation{ });
        m_image_views.emplace_back(emplaced_image.createView(device, ImageView::Info{ }));
    }
}

} // namespace core
