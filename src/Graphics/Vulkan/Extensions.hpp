#pragma once
#include <Core/Macro/Attributes.hpp>
#include "Version.hpp"

namespace graphics::vulkan {
    enum class VulkanExtension {
        // General
        Swapchain,           // VK_KHR_swapchain

        // Efficiency
        BufferDeviceAddress, // VK_KHR_buffer_device_address

        // Debug
        DebugUtils,          // VK_EXT_debug_utils
        MemoryBudget,        // VK_EXT_memory_budget

        VulkanExtensionsCount,
    };

    struct SupportedExtensions final {
        Version versions[static_cast<size_t>(VulkanExtension::VulkanExtensionsCount)];

        SupportedExtensions() noexcept;
        SupportedExtensions(void* physicalDevice);

        PURE bool hasExtension(VulkanExtension ext) const noexcept;
        PURE Version getExtensionVersion(VulkanExtension ext) const noexcept;
    };
    
    PURE bool hasExtension(VulkanExtension ext) noexcept;
    PURE Version getExtensionVersion(VulkanExtension ext) noexcept;
    PURE char const* getFullExtensionName(VulkanExtension ext) noexcept;

    void loadVkSupportedExtensionList();
    void updateVkSupportedExtensionListForDevice(void* physicalDevice);
} // namespace graphics::vulkan
