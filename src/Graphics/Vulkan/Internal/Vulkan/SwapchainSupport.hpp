#pragma once
#include <vulkan/vulkan.h>
#include <Core/Math/Vec.hpp>
#include <Core/Container/DynArray.hpp>

namespace graphics::vulkan::internal {
    struct SwapchainSupport final {
        VkSurfaceCapabilitiesKHR           const capabilities { };
        core::DynArray<VkSurfaceFormatKHR> const formats      { };
        core::DynArray<VkPresentModeKHR>   const presentModes { };

        SwapchainSupport() noexcept = default;
        SwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface);

        VkSurfaceFormatKHR const& chooseSurfaceFormat() const noexcept;
        VkPresentModeKHR          choosePresentMode  () const noexcept;
        VkExtent2D                chooseSwapExtent   (core::Vec2u32 pixelSize) const noexcept;
    };
} // namespace graphics::vulkan::internal
