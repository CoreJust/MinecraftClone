#pragma once
#include <vulkan/vulkan.h>

namespace graphics::vulkan {
    extern PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion;

    void loadInstancelessVkFunctions();
} // namespace graphics::vulkan
