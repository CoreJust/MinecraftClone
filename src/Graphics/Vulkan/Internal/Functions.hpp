#pragma once
#include <vulkan/vulkan.h>

namespace graphics::vulkan::internal {
    extern PFN_vkEnumerateInstanceVersion      pvkEnumerateInstanceVersion;
    extern PFN_vkCreateDebugUtilsMessengerEXT  pvkCreateDebugUtilsMessengerEXT;
    extern PFN_vkDestroyDebugUtilsMessengerEXT pvkDestroyDebugUtilsMessengerEXT;

    void loadInstancelessVkFunctions();
    void loadVkFunctions(VkInstance& instance);
} // namespace graphics::vulkan::internal
