#include <core/vulkan/Layers.hpp>

#include <core/IO/Log.hpp>
#include <core/vulkan/Check.hpp>
#include <core/vulkan/VulkanVersion.hpp>

#include <fmt/core.h>
#include <vulkan/vulkan.h>

#include <string>
#include <vector>
#include <unordered_map>

namespace core {

VulkanLayers::VulkanLayers() noexcept {
    memset(versions, 255, std::size(versions));
}

bool VulkanLayers::hasLayer(VulkanLayer const layer) const noexcept {
    return getLayerVersion(layer).epoch != UINT32_MAX;
}

Version VulkanLayers::getLayerVersion(VulkanLayer const layer) const noexcept {
    return versions[static_cast<size_t>(layer)];
}


std::string VulkanLayers::toString(std::string_view const indent) const {
    std::string layers_message;
    for (uint32_t i = 0; i < static_cast<uint32_t>(VulkanLayer::VulkanLayersCount); ++i) {
        VulkanLayer layer = static_cast<VulkanLayer>(i);
        if (hasLayer(layer)) {
            Version const v = getLayerVersion(layer);
            layers_message += fmt::format("{}{:40} v{}.{}.{}\n", indent, getFullLayerName(layer), v.major, v.minor, v.patch);
        }
    }
    return layers_message;
}

char const* getFullLayerName(VulkanLayer const layer) noexcept {
    static char const* LAYER_NAMES[] = {
        "VK_LAYER_KHRONOS_validation",
        "VK_LAYER_LUNARG_api_dump",
        "VK_LAYER_LUNARG_monitor",
        "VK_LAYER_KHRONOS_profiling",
    };
    return LAYER_NAMES[static_cast<size_t>(layer)];
}

VulkanLayers loadSupportedLayers() {
    VulkanLayers supported_layers{ };

    MC_INFO("Loading supported Vulkan layers list...");
    uint32_t layers_count = 0;
    VK_ASSERT(vkEnumerateInstanceLayerProperties(&layers_count, nullptr));

    std::vector<VkLayerProperties> layers(layers_count);
    VK_ASSERT(vkEnumerateInstanceLayerProperties(&layers_count, layers.data()));

    std::unordered_map<std::string, uint32_t> layer_versions;
    layer_versions.reserve(static_cast<size_t>(layers_count));
    for (VkLayerProperties const& layer : layers) {
        layer_versions[layer.layerName] = layer.specVersion;
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(VulkanLayer::VulkanLayersCount); ++i) {
        VulkanLayer layer = static_cast<VulkanLayer>(i);
        if (auto it = layer_versions.find(getFullLayerName(layer)); it != layer_versions.end()) {
            supported_layers.versions[static_cast<size_t>(layer)] = vkToVersion(it->second);
        }
    }

    return supported_layers;
}

} // namespace core
