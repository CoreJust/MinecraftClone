#include <core/vulkan/Surface.hpp>

// DONT_CHECK INCLUDE_ORDER
#include <volk.h>

namespace core::vk {

CORE_VK_RESOURCE_DESTROY_IMPL(RawSurface) {
    vkDestroySurfaceKHR(instance.handle(), self.m_handle, nullptr);
}

CORE_VK_RESOURCE_DEFERRED_CONSTRUCTION_IMPL(RawSurface, Instance const& instance, SurfaceProvider const& provider) {
    self.m_handle = provider.createSurface(instance.handle());
    CORE_VK_CAPTURE_DESTRUCTION_CONTEXT() { .instance = instance.raw() };
}

} // namespace core::vk
