#pragma once
#include <Core/Collection/DynArray.hpp>
#include <vulkan/vulkan.h>

namespace graphics::vulkan::internal {
    struct SwapchainSupport final {
        VkSurfaceCapabilitiesKHR                       const capabilities { };
        core::collection::DynArray<VkSurfaceFormatKHR> const formats      { };
        core::collection::DynArray<VkPresentModeKHR>   const presentModes { };

        SwapchainSupport() noexcept = default;
        SwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface);

        VkSurfaceFormatKHR const& chooseSurfaceFormat() const noexcept;
        VkPresentModeKHR          choosePresentMode  () const noexcept;
        VkExtent2D                chooseSwapExtent   (void* window) const noexcept;
    };
} // namespace graphics::vulkan::internal
