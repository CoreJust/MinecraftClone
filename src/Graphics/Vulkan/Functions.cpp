#include "Functions.hpp"
#include <Core/IO/Logger.hpp>

#define LOAD_FUNC_INSTANCELESS(name)                                             \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(VK_NULL_HANDLE, #name)); \
    core::io::info("Function " #name " : {}", (name == NULL ? "Not found" : "Found"))

namespace graphics::vulkan {
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion;

    void loadInstancelessVkFunctions() {
        core::io::info("Loading instanceless Vulkan functions...");
        LOAD_FUNC_INSTANCELESS(vkEnumerateInstanceVersion);
    }
} // namespace graphics::vulkan
