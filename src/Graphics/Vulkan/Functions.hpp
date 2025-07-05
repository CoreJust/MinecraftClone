#pragma once
#include <vulkan/vulkan.h>

namespace graphics::vulkan {
    extern PFN_vkEnumerateInstanceVersion      vkEnumerateInstanceVersion;
    extern PFN_vkCreateDebugUtilsMessengerEXT  vkCreateDebugUtilsMessengerEXT;
    extern PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT;

    void loadInstancelessVkFunctions();
    void loadVkFunctions(VkInstance& instance);
} // namespace graphics::vulkan
