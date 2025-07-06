#pragma once
#include <vulkan/vulkan.h>

namespace graphics::vulkan::internal {
    bool checkVkResult(VkResult result);
} // namespace graphics::vulkan::internal

#define VK_CHECK(...) ::graphics::vulkan::internal::checkVkResult(__VA_ARGS__)
