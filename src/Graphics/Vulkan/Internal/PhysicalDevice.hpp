#pragma once
#include <vector>
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>
#include "QueueFamilies.hpp"

namespace graphics::vulkan::internal {
    class Instance;
    class Surface;

    class PhysicalDevice final {
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties m_properties { };
        QueueFamilies m_queueFamilies { };

    private:
        PhysicalDevice(VkPhysicalDevice device, Surface& surface) noexcept;

    public:
        PhysicalDevice() noexcept = default;
        PhysicalDevice(PhysicalDevice&&) noexcept = default;
        PhysicalDevice(const PhysicalDevice&) noexcept = default;
        PhysicalDevice& operator=(PhysicalDevice&&) noexcept = default;
        PhysicalDevice& operator=(const PhysicalDevice&) noexcept = default;

        static std::vector<PhysicalDevice> getPhysicalDevices(Instance& instance, Surface& surface) noexcept;
        static PhysicalDevice choosePhysicalDevice(Instance& instance, Surface& surface);

        PURE VkPhysicalDevice get() const noexcept { return m_physicalDevice; }
        PURE VkPhysicalDeviceProperties const& props() const noexcept { return m_properties; }
        PURE QueueFamilies const& queueFamilies() const noexcept { return m_queueFamilies; }
    };
} // namespace graphics::vulkan::internal

