#include <core/vulkan/Surface.hpp>

#include <core/vulkan/Check.hpp>

// DONT_CHECK INCLUDE_ORDER
#include <volk.h>
// DONT_CHECK INCLUDE_ORDER
#include <GLFW/glfw3.h>

namespace core {

VKC_RESOURCE_DESTROY_IMPL(RawSurface) {
    vkDestroySurfaceKHR(instance.handle(), self.m_handle, nullptr);
}

VKC_RESOURCE_DEFERRED_CONSTRUCTION_IMPL(RawSurface, Instance const& instance, Window const& window) {
    VKC_ASSERT(glfwCreateWindowSurface(instance.handle(), window.nativeHandle(), nullptr, &self.m_handle));
    VKC_CAPTURE_DESTRUCTION_CONTEXT() { .instance = instance.raw() };
}

} // namespace core
