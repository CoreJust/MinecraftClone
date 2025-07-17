#pragma once
#include <vulkan/vulkan.h>

namespace graphics::vulkan::internal {
    struct SwapchainFormat final {
        VkSurfaceFormatKHR surfaceFormat;
        VkPresentModeKHR presentMode;
        VkExtent2D extent;
    };
} // namespace graphics::vulkan::internal
