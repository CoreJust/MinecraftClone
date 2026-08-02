#pragma once

#include <volk.h>

struct GLFWwindow;

namespace core::vk {

// Creates a VkSurfaceKHR for the given window in a platform-specific way.
// On macOS (MoltenVK) it reuses a persistent CAMetalLayer attached to the
// window's view, so recreating the surface (e.g. on hot reload) does not
// attach a fresh layer and blank the window. On other platforms it delegates
// to glfwCreateWindowSurface. The MoltenVK specifics live in MetalLayer.
[[nodiscard]]
VkSurfaceKHR createWindowSurface(VkInstance instance, GLFWwindow* window);

} // namespace core::vk
