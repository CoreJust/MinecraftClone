#pragma once

#include <core/common/Version.hpp>

namespace core {

enum class VulkanLayer {
    // Debug
    Validation, // VK_LAYER_KHRONOS_validation
    ApiDump,    // VK_LAYER_LUNARG_api_dump
    Monitor,    // VK_LAYER_LUNARG_monitor
    Profiling,  // VK_LAYER_KHRONOS_profiling

    VulkanLayersCount,
};

[[nodiscard]]
bool hasLayer(VulkanLayer const layer) noexcept;

[[nodiscard]]
Version getLayerVersion(VulkanLayer const layer) noexcept;

[[nodiscard]]
char const* getFullLayerName(VulkanLayer const layer) noexcept;

void loadVkSupportedLayerList();

} // namespace core
