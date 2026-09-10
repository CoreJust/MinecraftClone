#pragma once

#include <core/common/Version.hpp>
#include <core/vulkan/enum/VulkanEnum.hpp>

#include <string>

namespace core::vk {

enum class VulkanLayer {
    // Debug
    Validation, // VK_LAYER_KHRONOS_validation
    ApiDump,    // VK_LAYER_LUNARG_api_dump
    Monitor,    // VK_LAYER_LUNARG_monitor
    Profiling,  // VK_LAYER_KHRONOS_profiling

    Count,
};

struct VulkanLayers final {
    Version versions[countOf<VulkanLayer>()];

    VulkanLayers() noexcept;

    [[nodiscard]]
    bool hasLayer(VulkanLayer const layer) const noexcept;

    [[nodiscard]]
    Version getLayerVersion(VulkanLayer const layer) const noexcept {
        return versions[indexOf(layer)];
    }

    [[nodiscard]]
    Version& versionAt(VulkanLayer const layer) & noexcept {
        return versions[indexOf(layer)];
    }

    [[nodiscard]]
    std::string toString(std::string_view const indent = "") const;
};

[[nodiscard]]
char const* getFullLayerName(VulkanLayer const layer) noexcept;

VulkanLayers loadSupportedLayers();

} // namespace core::vk

CORE_VK_REGISTER_ENUM(VulkanLayer);
