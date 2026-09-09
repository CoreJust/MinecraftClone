#pragma once

#include <core/vulkan/SurfaceProvider.hpp>

#include <vector>

namespace core {
class Window;
}

namespace core::vk {

class GlfwSurfaceProvider final : public SurfaceProvider {
public:
    explicit GlfwSurfaceProvider(Window const& window);

    [[nodiscard]]
    std::span<VulkanExtension const> requiredInstanceExtensions() const noexcept override;
    [[nodiscard]]
    Extent2d framebufferExtent() const noexcept override;
    [[nodiscard]]
    bool isFramebufferExtentZero() const noexcept override;
    [[nodiscard]]
    VkSurfaceKHR createSurface(VkInstance instance) const override;
private:
    Window const* m_window;
    std::vector<VulkanExtension> m_required_instance_extensions;
};

} // namespace core::vk
