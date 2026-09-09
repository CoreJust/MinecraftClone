#include <core/vulkan/GlfwSurfaceProvider.hpp>

#include <core/vulkan/builder/InstanceBuilder.hpp>
#include <core/window/Window.hpp>

#include <GLFW/glfw3.h>

namespace core::vk {

GlfwSurfaceProvider::GlfwSurfaceProvider(Window const& window)
    : m_window(&window)
{
    if (glfwVulkanSupported() != GLFW_TRUE) {
        throw InstanceCreationError(InstanceCreationError::GlfwVulkanNotSupported);
    }

    uint32_t extension_count = 0;
    char const** const extensions = glfwGetRequiredInstanceExtensions(&extension_count);
    if (extensions == nullptr || extension_count == 0) {
        throw InstanceCreationError(InstanceCreationError::GlfwVulkanNotSupported);
    }

    m_required_instance_extensions.reserve(extension_count);
    for (uint32_t i = 0; i < extension_count; ++i) {
        if (auto maybe_extension = extensionFromFullName(extensions[i])) {
            m_required_instance_extensions.push_back(*maybe_extension);
        } else {
            throw InstanceCreationError(
                InstanceCreationError::MissingRequiredExtension,
                "{} not recognized", extensions[i]
            );
        }
    }
}

std::span<VulkanExtension const> GlfwSurfaceProvider::requiredInstanceExtensions() const noexcept {
    return m_required_instance_extensions;
}

Extent2d GlfwSurfaceProvider::framebufferExtent() const noexcept {
    auto const [width, height] = m_window->framebufferSize();
    return { width, height };
}

bool GlfwSurfaceProvider::isFramebufferExtentZero() const noexcept {
    return m_window->isFramebufferSizeZero();
}

} // namespace core::vk
