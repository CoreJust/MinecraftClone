#include "Surface.hpp"
#include <vulkan/vulkan.h>
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include <Graphics/Window/Window.hpp>
#include "../Check.hpp"
#include "../../Exception.hpp"

namespace graphics::vulkan::internal {
    Surface::Surface(window::Window& win, Instance& instance)
        : m_instance(instance.get()) {
        core::note("Creating Vulkan surface...");
        auto createVkSurface = reinterpret_cast<VkResult(*)(VkInstance, void*, const VkAllocationCallbacks*, VkSurfaceKHR*)>(win.getSurfaceCreateCallback());
        if (!VK_CHECK(createVkSurface(m_instance, win.windowImpl(), nullptr, &m_surface))) {
            core::fatal("Failed to create Vulkan surface");
            throw VulkanException { };
        }
    }

    Surface::~Surface() {
        if (m_surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
            m_surface = VK_NULL_HANDLE;
            core::note("Destroyed Vulkan surface");
        }
    }

    bool Surface::isSupportedOn(VkPhysicalDevice physicalDevice, u32 queueIndex) const {
        ASSERT(m_surface != VK_NULL_HANDLE);
        VkBool32 result = false;
        if (!VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueIndex, m_surface, &result)))
            return false;
        return result;
    }
} // namespace graphics::vulkan::internal
