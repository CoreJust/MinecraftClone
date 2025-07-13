#include "Functions.hpp"
#include <Core/IO/Logger.hpp>

#define LOAD_FUNC_INSTANCELESS(name)                                                   \
    p##name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(VK_NULL_HANDLE, #name)); \
    core::io::info("Function " #name " : {}", (name == NULL ? "Not found" : "Found"))
#define LOAD_FUNC(name)                                                                \
    p##name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));       \
    core::io::info("Function " #name " : {}", (name == NULL ? "Not found" : "Found"))

namespace graphics::vulkan::internal {
    PFN_vkEnumerateInstanceVersion      pvkEnumerateInstanceVersion      = nullptr;
    PFN_vkCreateDebugUtilsMessengerEXT  pvkCreateDebugUtilsMessengerEXT  = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT pvkDestroyDebugUtilsMessengerEXT = nullptr;

    void loadInstancelessVkFunctions() {
        core::io::info("Loading instanceless Vulkan functions...");
        LOAD_FUNC_INSTANCELESS(vkEnumerateInstanceVersion);
    }

    void loadVkFunctions(VkInstance& instance) {
        core::io::info("Loading Vulkan functions...");
        LOAD_FUNC(vkCreateDebugUtilsMessengerEXT);
        LOAD_FUNC(vkDestroyDebugUtilsMessengerEXT);
    }
} // namespace graphics::vulkan::internal
