#pragma once
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>
#include "Instance.hpp"

namespace graphics::window {
    class Window;
}

namespace graphics::vulkan::internal {
    class Surface final {
        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
        VkInstance m_instance = VK_NULL_HANDLE; // The instance used for surface creation

    public:
        Surface(window::Window& win, Instance& instance);
        ~Surface();

        PURE bool isSupportedOn(VkPhysicalDevice physicalDevice, u32 queueIndex) const;

        PURE VkSurfaceKHR get() const noexcept { return m_surface; }
    };
} // namespace graphics::vulkan::internal
