#pragma once

#include <core/common/Version.hpp>
#include <core/meta/Enum.hpp>

#include <string>

namespace core {

enum class VulkanLayer {
    // Debug
    Validation, // VK_LAYER_KHRONOS_validation
    ApiDump,    // VK_LAYER_LUNARG_api_dump
    Monitor,    // VK_LAYER_LUNARG_monitor
    Profiling,  // VK_LAYER_KHRONOS_profiling

    Count,
};

CORE_ENUM_FUNCTIONS(VulkanLayer);

struct VulkanLayers final {
    Version versions[static_cast<size_t>(VulkanLayer::Count)];

    VulkanLayers() noexcept;

    [[nodiscard]]
    bool hasLayer(VulkanLayer const layer) const noexcept;

    [[nodiscard]]
    Version getLayerVersion(VulkanLayer const layer) const noexcept {
        return versions[static_cast<size_t>(layer)];
    }

    [[nodiscard]]
    Version& versionAt(VulkanLayer const layer) & noexcept {
        return versions[static_cast<size_t>(layer)];
    }

    [[nodiscard]]
    std::string toString(std::string_view const indent = "") const;
};

[[nodiscard]]
char const* getFullLayerName(VulkanLayer const layer) noexcept;

VulkanLayers loadSupportedLayers();

} // namespace core
