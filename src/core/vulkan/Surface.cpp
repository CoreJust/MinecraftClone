#include <core/vulkan/Surface.hpp>

#include <core/vulkan/Check.hpp>
#include <core/vulkan/SurfacePlatform.hpp>

// DONT_CHECK INCLUDE_ORDER
#include <volk.h>

namespace core::vk {

CORE_VK_RESOURCE_DESTROY_IMPL(RawSurface) {
    vkDestroySurfaceKHR(instance.handle(), self.m_handle, nullptr);
}

CORE_VK_RESOURCE_DEFERRED_CONSTRUCTION_IMPL(RawSurface, Instance const& instance, Window const& window) {
    self.m_handle = createWindowSurface(instance.handle(), window.nativeHandle());
    CORE_VK_CAPTURE_DESTRUCTION_CONTEXT() { .instance = instance.raw() };
}

} // namespace core::vk
