#include <core/vulkan/SurfacePlatform.hpp>

#include <core/macro/OS.hpp>
#include <core/vulkan/MetalLayer.hpp>

#ifdef OSX

namespace core::vk {

VkSurfaceKHR createWindowSurface(VkInstance const instance, GLFWwindow* const window) {
    return MetalLayer{ window }.createSurface(instance);
}

} // namespace core::vk

#endif // OSX
