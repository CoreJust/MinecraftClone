#pragma once
#include "Version.hpp"

namespace graphics::vulkan {
    enum class VulkanLayer {
        // Debug
        Validation, // VK_LAYER_KHRONOS_validation

        VulkanLayersCount,
    };

    bool hasLayer(VulkanLayer layer) noexcept;
    Version getLayerVersion(VulkanLayer layer) noexcept;

    char const* getFullLayerName(VulkanLayer layer) noexcept;

    void loadVkSupportedLayerList();
} // namespace graphics::vulkan
