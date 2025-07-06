#include "Surface.hpp"
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include "Check.hpp"
#include "../Exception.hpp"

namespace graphics::vulkan::internal {
    Surface::Surface(void* window, void* surfaceCreator, Instance& instance) : m_instance(instance.get()) {
        core::io::info("Creating Vulkan surface...");
        auto createVkSurface = reinterpret_cast<VkResult(*)(void*, VkInstance, VkSurfaceKHR*)>(surfaceCreator);
        if (!VK_CHECK(createVkSurface(window, m_instance, &m_surface))) {
            core::io::fatal("Failed to create Vulkan surface");
            throw VulkanException { };
        }
    }

    Surface::~Surface() {
        if (m_surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
            m_surface = VK_NULL_HANDLE;
            core::io::info("Destroyed Vulkan surface");
        }
    }

    bool Surface::isSupportedOn(VkPhysicalDevice physicalDevice, uint32_t queueIndex) const {
        ASSERT(m_surface != VK_NULL_HANDLE);
        VkBool32 result = false;
        if (!VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueIndex, m_surface, &result)))
            return false;
        return result;
    }
} // namespace graphics::vulkan::internal
