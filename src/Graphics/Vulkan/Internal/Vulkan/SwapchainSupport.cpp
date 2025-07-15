#include "SwapchainSupport.hpp"
#include <algorithm>
#include <Core/Common/Assert.hpp>

namespace graphics::vulkan::internal {
namespace {
    VkSurfaceCapabilitiesKHR getVkSurfaceCapabilities(VkPhysicalDevice device, VkSurfaceKHR surface) {
        VkSurfaceCapabilitiesKHR result;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &result);
        return result;
    }

    core::DynArray<VkSurfaceFormatKHR> getVkSurfaceFormats(VkPhysicalDevice device, VkSurfaceKHR surface) {
        u32 formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);

        if (formatCount != 0) {
            core::DynArray<VkSurfaceFormatKHR> result(formatCount);
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, result.data());
            return result;
        }
        return { };
    }

    core::DynArray<VkPresentModeKHR> getVkPresentModes(VkPhysicalDevice device, VkSurfaceKHR surface) {
        u32 presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);

        if (presentModeCount != 0) {
            core::DynArray<VkPresentModeKHR> result(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, result.data());
            return result;
        }
        return { };
    }
} // namespace

    SwapchainSupport::SwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface) 
        : capabilities(getVkSurfaceCapabilities(device, surface))
        , formats     (getVkSurfaceFormats     (device, surface))
        , presentModes(getVkPresentModes       (device, surface))
    { }

    VkSurfaceFormatKHR const& SwapchainSupport::chooseSurfaceFormat() const noexcept {
        ASSERT(formats.size() != 0);
        auto it = std::find_if(formats.begin(), formats.end(), [](VkSurfaceFormatKHR const& fmt) {
            return fmt.format == VK_FORMAT_B8G8R8A8_SRGB && fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (it != formats.end())
            return *it;
        return formats[0];
    }
        
    VkPresentModeKHR SwapchainSupport::choosePresentMode() const noexcept {
        ASSERT(presentModes.size() != 0);
        auto it = std::find_if(presentModes.begin(), presentModes.end(), [](VkPresentModeKHR const& mode) {
            return mode == VK_PRESENT_MODE_MAILBOX_KHR;
        });
        if (it != presentModes.end())
            return *it;
        return VK_PRESENT_MODE_FIFO_KHR; // It is guaranteed to be supported.
    }
    
    VkExtent2D SwapchainSupport::chooseSwapExtent(core::Vec2u32 pixelSize) const noexcept {
        return VkExtent2D { 
            std::clamp<u32>(pixelSize[0], capabilities.minImageExtent.width,  capabilities.maxImageExtent.width),
            std::clamp<u32>(pixelSize[1], capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
        };
    }
} // namespace graphics::vulkan::internal
