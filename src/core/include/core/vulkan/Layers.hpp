#pragma once

#include <core/common/Version.hpp>

#include <string>

namespace core {

enum class VulkanLayer {
    // Debug
    Validation, // VK_LAYER_KHRONOS_validation
    ApiDump,    // VK_LAYER_LUNARG_api_dump
    Monitor,    // VK_LAYER_LUNARG_monitor
    Profiling,  // VK_LAYER_KHRONOS_profiling

    VulkanLayersCount,
};

struct VulkanLayers final {
    Version versions[static_cast<size_t>(VulkanLayer::VulkanLayersCount)];

    VulkanLayers() noexcept;

    [[nodiscard]]
    bool hasLayer(VulkanLayer const ext) const noexcept;
    [[nodiscard]]
    Version getLayerVersion(VulkanLayer const ext) const noexcept;

    [[nodiscard]]
    std::string toString(std::string_view const indent = "") const;
};

[[nodiscard]]
char const* getFullLayerName(VulkanLayer const layer) noexcept;

VulkanLayers loadSupportedLayers();

} // namespace core
