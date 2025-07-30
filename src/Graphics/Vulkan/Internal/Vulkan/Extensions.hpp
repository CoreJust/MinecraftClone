#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Graphics/Vulkan/Version.hpp>

namespace graphics::vulkan::internal {
    enum class VulkanExtension {
        // General
        Swapchain,            // VK_KHR_swapchain
        Maintenance4,         // VK_KHR_maintenance4
        Maintenance5,         // VK_KHR_maintenance5

        // Efficiency
        DedicatedAllocation,  // VK_KHR_dedicated_allocation
        BufferDeviceAddress,  // VK_KHR_buffer_device_address
        BindMemory2,          // VK_KHR_bind_memory2
        MemoryPriority,       // VK_EXT_memory_priority

        // Debug
        DebugUtils,           // VK_EXT_debug_utils
        MemoryBudget,         // VK_EXT_memory_budget
        DeviceCoherentMemory, // VK_AMD_device_coherent_memory 

        VulkanExtensionsCount,
    };

    class PhysicalDevice;

    struct SupportedExtensions final {
        Version versions[static_cast<usize>(VulkanExtension::VulkanExtensionsCount)];

        SupportedExtensions() noexcept;
        SupportedExtensions(PhysicalDevice& physicalDevice);

        PURE bool hasExtension(VulkanExtension ext) const noexcept;
        PURE Version getExtensionVersion(VulkanExtension ext) const noexcept;
    };
    
    PURE bool hasExtension(VulkanExtension ext) noexcept;
    PURE Version getExtensionVersion(VulkanExtension ext) noexcept;
    PURE char const* getFullExtensionName(VulkanExtension ext) noexcept;

    void loadVkSupportedExtensionList();
    void updateVkSupportedExtensionListForDevice(PhysicalDevice& physicalDevice);
} // namespace graphics::vulkan::internal
