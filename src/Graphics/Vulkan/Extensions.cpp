#include "Extensions.hpp"
#include <string>
#include <format>
#include <vector>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <Core/IO/Logger.hpp>

namespace graphics::vulkan {
    Version g_supportedExtensions[static_cast<size_t>(VulkanExtension::VulkanExtensionsCount)];

    bool hasExtension(VulkanExtension ext) noexcept {
        return getExtensionVersion(ext).variant != UINT32_MAX;
    }

    Version getExtensionVersion(VulkanExtension ext) noexcept {
        return g_supportedExtensions[static_cast<size_t>(ext)];
    }

    char const* getFullExtensionName(VulkanExtension ext) noexcept {
        char const* EXTENSION_NAMES[] = {
            "VK_KHR_buffer_device_address",
            "VK_EXT_debug_utils",
            "VK_EXT_memory_budget",
        };
        return EXTENSION_NAMES[static_cast<size_t>(ext)];
    }

    void loadVkSupportedExtensionList() {
        core::io::info("Loading supported Vulkan extensions list...");
        uint32_t extensionCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);

        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());

        std::string extensionsMessage;
        std::unordered_map<std::string, uint32_t> extensionVersions;
        for (const VkExtensionProperties& ext : extensions) {
            extensionsMessage += std::format("\t{:40} v{}\n", ext.extensionName, ext.specVersion);
            extensionVersions[ext.extensionName] = ext.specVersion;
        }

        core::io::info("Found Vulkan extensions:\n{}", extensionsMessage);

        memset(g_supportedExtensions, 255, std::size(g_supportedExtensions));
#define CHECK_EXTENSION(ext)                                                              \
            if (auto it = extensionVersions.find(getFullExtensionName(VulkanExtension:: ext)); it != extensionVersions.end())         \
                g_supportedExtensions[static_cast<size_t>(VulkanExtension:: ext)] = Version::fromVk(it->second)
        CHECK_EXTENSION(BufferDeviceAddress);
        CHECK_EXTENSION(DebugUtils);
        CHECK_EXTENSION(MemoryBudget);
#undef CHECK_EXTENSION
    }
} // namespace graphics::vulkan
