#pragma once
#include <vulkan/vulkan.h>

namespace graphics::vulkan {
    bool checkVkResult(VkResult result);
} // namespace graphics::vulkan

#define VK_CHECK(...) ::graphics::vulkan::checkVkResult(__VA_ARGS__)
