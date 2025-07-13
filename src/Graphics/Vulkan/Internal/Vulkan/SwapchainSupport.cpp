#include "SwapchainSupport.hpp"
#include <algorithm>
#include <GLFW/glfw3.h>
#include <Core/Common/Assert.hpp>

namespace graphics::vulkan::internal {
namespace {
    VkSurfaceCapabilitiesKHR getVkSurfaceCapabilities(VkPhysicalDevice device, VkSurfaceKHR surface) {
        VkSurfaceCapabilitiesKHR result;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &result);
        return result;
    }

    core::collection::DynArray<VkSurfaceFormatKHR> getVkSurfaceFormats(VkPhysicalDevice device, VkSurfaceKHR surface) {
        uint32_t formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);

        if (formatCount != 0) {
            core::collection::DynArray<VkSurfaceFormatKHR> result(formatCount);
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, result.data());
            return result;
        }
        return { };
    }

    core::collection::DynArray<VkPresentModeKHR> getVkPresentModes(VkPhysicalDevice device, VkSurfaceKHR surface) {
        uint32_t presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);

        if (presentModeCount != 0) {
            core::collection::DynArray<VkPresentModeKHR> result(presentModeCount);
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
    
    VkExtent2D SwapchainSupport::chooseSwapExtent(void* window) const noexcept {
        int iwidth, iheight;
        glfwGetWindowSize(reinterpret_cast<GLFWwindow*>(window), &iwidth, &iheight);
        uint32_t width  = iwidth  < 0 ? 0 : static_cast<uint32_t>(iwidth);
        uint32_t height = iheight < 0 ? 0 : static_cast<uint32_t>(iheight);
        return VkExtent2D { 
            std::clamp<uint32_t>(width,  capabilities.minImageExtent.width,  capabilities.maxImageExtent.width),
            std::clamp<uint32_t>(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
        };
    }
} // namespace graphics::vulkan::internal
