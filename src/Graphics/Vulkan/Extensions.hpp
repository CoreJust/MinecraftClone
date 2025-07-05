#pragma once
#include "Version.hpp"

namespace graphics::vulkan {
    enum class VulkanExtension {
        // Efficiency
        BufferDeviceAddress, // VK_KHR_buffer_device_address

        // Debug
        DebugUtils,          // VK_EXT_debug_utils
        MemoryBudget,        // VK_EXT_memory_budget

        VulkanExtensionsCount,
    };

    bool hasExtension(VulkanExtension ext) noexcept;
    Version getExtensionVersion(VulkanExtension ext) noexcept;

    void loadVkSupportedExtensionList();
} // namespace graphics::vulkan
