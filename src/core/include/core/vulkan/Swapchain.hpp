#pragma once

#include <core/vulkan/Device.hpp>
#include <core/vulkan/Image.hpp>

namespace core::vk {

class RawSwapchain : public VulkanResourceBase<VkSwapchainKHR> {
    CORE_VK_RESOURCE_CONTEXT(RawSwapchain,
        VkDevice device_handle;);
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(VkSwapchainKHR const swapchain, Device const& device) {
        CORE_VK_CAPTURE_DESTRUCTION_CONTEXT(){ .device_handle = device.handle() };
        self.m_handle = swapchain;
    }
};

class Swapchain : public VulkanRaii<RawSwapchain> {
public:
    Swapchain() noexcept = default;
    Swapchain(VkSwapchainKHR const swapchain, Image::Info const& info, Device const& device);

    [[nodiscard]]
    constexpr std::vector<Image> const& images() const noexcept { return m_images; }
    [[nodiscard]]
    constexpr std::vector<ImageView> const& imageViews() const noexcept { return m_image_views; }
private:
    std::vector<Image> m_images;
    std::vector<ImageView> m_image_views;
};

} // namespace core::vk
