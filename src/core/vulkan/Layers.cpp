#include <core/vulkan/Layers.hpp>

#include <core/IO/Log.hpp>
#include <core/vulkan/Check.hpp>
#include <core/vulkan/VulkanVersion.hpp>

#include <vulkan/vulkan.h>

#include <string>
#include <format>
#include <vector>
#include <unordered_map>

namespace core {

Version g_supported_layers[static_cast<size_t>(VulkanLayer::VulkanLayersCount)];

bool hasLayer(VulkanLayer const layer) noexcept {
    return getLayerVersion(layer).epoch != UINT32_MAX;
}

Version getLayerVersion(VulkanLayer const layer) noexcept {
    return g_supported_layers[static_cast<size_t>(layer)];
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

void loadVkSupportedLayerList() {
    MC_INFO("Loading supported Vulkan layers list...");
    uint32_t layers_count = 0;
    VK_ASSERT(vkEnumerateInstanceLayerProperties(&layers_count, nullptr));

    std::vector<VkLayerProperties> layers(layers_count);
    VK_ASSERT(vkEnumerateInstanceLayerProperties(&layers_count, layers.data()));

    std::string layers_message;
    std::unordered_map<std::string, uint32_t> layer_versions;
    layer_versions.reserve(static_cast<size_t>(VulkanLayer::VulkanLayersCount));
    for (const VkLayerProperties& layer : layers) {
        Version v = vkToVersion(layer.specVersion);
        layers_message += std::format("\t{:40} v{}.{},{}\n", layer.layerName, v.major, v.minor, v.patch);
        layer_versions[layer.layerName] = layer.specVersion;
    }

    MC_INFO("Found Vulkan layers:\n{}", layers_message);

    memset(g_supported_layers, 255, std::size(g_supported_layers));
    for (int i = 0; i < static_cast<int>(VulkanLayer::VulkanLayersCount); ++i) {
        VulkanLayer layer = static_cast<VulkanLayer>(i);
        if (auto it = layer_versions.find(getFullLayerName(layer)); it != layer_versions.end()) {
            g_supported_layers[static_cast<size_t>(layer)] = vkToVersion(it->second);
        }
    }
}

} // namespace core
