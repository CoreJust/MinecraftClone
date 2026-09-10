#include <core/vulkan/GlfwSurfaceProvider.hpp>

#include <core/macro/OS.hpp>
#include <core/vulkan/MetalLayer.hpp>
#include <core/window/Window.hpp>

#ifdef OSX

namespace core::vk {

VkSurfaceKHR GlfwSurfaceProvider::createSurface(VkInstance const instance) const {
    return MetalLayer{ m_window->nativeHandle() }.createSurface(instance);
}

} // namespace core::vk

#endif // OSX
