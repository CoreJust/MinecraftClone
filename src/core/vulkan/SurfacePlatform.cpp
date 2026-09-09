#include <core/vulkan/GlfwSurfaceProvider.hpp>

#include <core/macro/OS.hpp>
#include <core/vulkan/Check.hpp>
#include <core/window/Window.hpp>

#ifndef OSX

#include <GLFW/glfw3.h>

namespace core::vk {

VkSurfaceKHR GlfwSurfaceProvider::createSurface(VkInstance const instance) const {
    VkSurfaceKHR handle = VK_NULL_HANDLE;
    CORE_VK_ASSERT(glfwCreateWindowSurface(instance, m_window->nativeHandle(), nullptr, &handle));
    return handle;
}

} // namespace core::vk

#endif // OSX
