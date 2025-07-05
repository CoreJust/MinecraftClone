#include "Functions.hpp"
#include <Core/IO/Logger.hpp>

#define LOAD_FUNC_INSTANCELESS(name)                                                   \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(VK_NULL_HANDLE, #name)); \
    core::io::info("Function " #name " : {}", (name == NULL ? "Not found" : "Found"))
#define LOAD_FUNC(name)                                                                \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));       \
    core::io::info("Function " #name " : {}", (name == NULL ? "Not found" : "Found"))

namespace graphics::vulkan {
    PFN_vkEnumerateInstanceVersion      vkEnumerateInstanceVersion      = nullptr;
    PFN_vkCreateDebugUtilsMessengerEXT  vkCreateDebugUtilsMessengerEXT  = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT = nullptr;

    void loadInstancelessVkFunctions() {
        core::io::info("Loading instanceless Vulkan functions...");
        LOAD_FUNC_INSTANCELESS(vkEnumerateInstanceVersion);
    }

    void loadVkFunctions(VkInstance& instance) {
        core::io::info("Loading Vulkan functions...");
        LOAD_FUNC(vkCreateDebugUtilsMessengerEXT);
        LOAD_FUNC(vkDestroyDebugUtilsMessengerEXT);
    }
} // namespace graphics::vulkan
