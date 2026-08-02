#include <core/vulkan/SurfacePlatform.hpp>

#include <core/macro/OS.hpp>
#include <core/vulkan/Check.hpp>

#ifndef OSX

#include <GLFW/glfw3.h>

namespace core::vk {

VkSurfaceKHR createWindowSurface(VkInstance const instance, GLFWwindow* const window) {
    VkSurfaceKHR handle = VK_NULL_HANDLE;
    CORE_VK_ASSERT(glfwCreateWindowSurface(instance, window, nullptr, &handle));
    return handle;
}

} // namespace core::vk

#endif // OSX
