#pragma once
#include <vector>
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>
#include "QueueFamilies.hpp"
#include "SwapchainSupport.hpp"
#include "../Extensions.hpp"

namespace graphics::vulkan::internal {
    class Instance;
    class Surface;

    class PhysicalDevice final {
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties m_properties { };
        QueueFamilies m_queueFamilies { };
        SupportedExtensions m_extensions { };
        SwapchainSupport m_swapchainSupport { };

    private:
        PhysicalDevice(VkPhysicalDevice device, Surface& surface) noexcept;

    public:
        static std::vector<PhysicalDevice> getPhysicalDevices(Instance& instance, Surface& surface) noexcept;
        static PhysicalDevice choosePhysicalDevice(Instance& instance, Surface& surface);

        PURE VkPhysicalDevice get() const noexcept { return m_physicalDevice; }
        PURE VkPhysicalDeviceProperties const& props()            const noexcept { return m_properties; }
        PURE QueueFamilies              const& queueFamilies()    const noexcept { return m_queueFamilies; }
        PURE SupportedExtensions        const& extensions()       const noexcept { return m_extensions; }
        PURE SwapchainSupport           const& swapchainSupport() const noexcept { return m_swapchainSupport; }
    };
} // namespace graphics::vulkan::internal

