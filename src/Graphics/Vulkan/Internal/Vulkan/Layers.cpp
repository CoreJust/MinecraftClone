#include "Layers.hpp"
#include <string>
#include <format>
#include <vector>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <Core/IO/Logger.hpp>

namespace graphics::vulkan::internal {
    Version g_supportedLayers[static_cast<usize>(VulkanLayer::VulkanLayersCount)];

    bool hasLayer(VulkanLayer layer) noexcept {
        return getLayerVersion(layer).variant != UINT32_MAX;
    }

    Version getLayerVersion(VulkanLayer layer) noexcept {
        return g_supportedLayers[static_cast<usize>(layer)];
    }

    char const* getFullLayerName(VulkanLayer layer) noexcept {
        static char const* LAYER_NAMES[] = {
            "VK_LAYER_KHRONOS_validation",
        };
        return LAYER_NAMES[static_cast<usize>(layer)];
    }

    void loadVkSupportedLayerList() {
        core::io::info("Loading supported Vulkan layers list...");
        u32 layersCount = 0;
        vkEnumerateInstanceLayerProperties(&layersCount, nullptr);

        std::vector<VkLayerProperties> layers(layersCount);
        vkEnumerateInstanceLayerProperties(&layersCount, layers.data());

        std::string layersMessage;
        std::unordered_map<std::string, u32> layerVersions;
        for (const VkLayerProperties& layer : layers) {
            Version v = Version::fromVk(layer.specVersion);
            layersMessage += std::format("\t{:40} v{}.{},{}\n", layer.layerName, v.major, v.minor, v.patch);
            layerVersions[layer.layerName] = layer.specVersion;
        }

        core::io::info("Found Vulkan layers:\n{}", layersMessage);

        memset(g_supportedLayers, 255, std::size(g_supportedLayers));
        for (int i = 0; i < static_cast<int>(VulkanLayer::VulkanLayersCount); ++i) {
            VulkanLayer layer = static_cast<VulkanLayer>(i);
            if (auto it = layerVersions.find(getFullLayerName(layer)); it != layerVersions.end())
                g_supportedLayers[static_cast<usize>(layer)] = Version::fromVk(it->second);
        }
    }
} // namespace graphics::vulkan::internal
