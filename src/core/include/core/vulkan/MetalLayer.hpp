#pragma once

#include <core/macro/OS.hpp>

#ifdef OSX

#include <volk.h>

struct GLFWwindow;

namespace core::vk {

// Owns a reference to the window's CAMetalLayer.
//
// The layer is created and attached to the window's content view on first use,
// then reused by every subsequent MetalLayer for the same window (the view
// retains it). This keeps Vulkan surface recreation, e.g. during a hot reload,
// from replacing the view's layer and blanking the window for a frame. The
// Metal specifics are confined to this class; the rest of core::vk only sees a
// VkSurfaceKHR.
class MetalLayer final {
public:
    explicit MetalLayer(GLFWwindow* window) noexcept;
    ~MetalLayer() noexcept;

    MetalLayer(MetalLayer const&) = delete;
    MetalLayer& operator=(MetalLayer const&) = delete;

    [[nodiscard]]
    VkSurfaceKHR createSurface(VkInstance instance) const noexcept;
private:
    void* m_layer; // CAMetalLayer*, retained
};

} // namespace core::vk

#endif // OSX
