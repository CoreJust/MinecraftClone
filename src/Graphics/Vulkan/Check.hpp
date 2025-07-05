#pragma once
#include <source_location>
#include <vulkan/vulkan.h>

namespace graphics::vulkan {
    bool checkVkResult(VkResult result, const std::source_location& loc = std::source_location::current());
} // namespace graphics::vulkan

#define VK_CHECK(...) ::graphics::vulkan::checkVkResult(__VA_ARGS__)
