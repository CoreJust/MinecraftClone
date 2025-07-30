#include "Extensions.hpp"
#include <string>
#include <format>
#include <vector>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <Core/IO/Logger.hpp>
#include "../Check.hpp"
#include "PhysicalDevice.hpp"

namespace graphics::vulkan::internal {
    SupportedExtensions g_supportedExtensions { };
        
    SupportedExtensions::SupportedExtensions() noexcept {
        memset(versions, 255, std::size(versions));
    }
    
    SupportedExtensions::SupportedExtensions(PhysicalDevice& physicalDevice) : SupportedExtensions() {
        u32 extensionCount = 0;
        VK_CHECK(vkEnumerateDeviceExtensionProperties(physicalDevice.get(), nullptr, &extensionCount, nullptr));

        std::vector<VkExtensionProperties> extensions(extensionCount);
        VK_CHECK(vkEnumerateDeviceExtensionProperties(physicalDevice.get(), nullptr, &extensionCount, extensions.data()));

        std::unordered_map<std::string, u32> extensionVersions;
        for (const VkExtensionProperties& ext : extensions)
            extensionVersions[ext.extensionName] = ext.specVersion;

        for (int i = 0; i < static_cast<int>(VulkanExtension::VulkanExtensionsCount); ++i) {
            VulkanExtension ext = static_cast<VulkanExtension>(i);
            if (auto it = extensionVersions.find(getFullExtensionName(ext)); it != extensionVersions.end())
                versions[static_cast<usize>(ext)] = Version::fromVk(it->second);
        }
    }

    bool SupportedExtensions::hasExtension(VulkanExtension ext) const noexcept {
        return getExtensionVersion(ext).variant != UINT32_MAX;
    }

    Version SupportedExtensions::getExtensionVersion(VulkanExtension ext) const noexcept {
        return versions[static_cast<usize>(ext)];
    }

    bool hasExtension(VulkanExtension ext) noexcept {
        return g_supportedExtensions.hasExtension(ext);
    }

    Version getExtensionVersion(VulkanExtension ext) noexcept {
        return g_supportedExtensions.getExtensionVersion(ext);
    }

    char const* getFullExtensionName(VulkanExtension ext) noexcept {
        char const* EXTENSION_NAMES[] = {
            "VK_KHR_swapchain",
            "VK_KHR_maintenance4",
            "VK_KHR_maintenance5",
            "VK_KHR_dedicated_allocation",
            "VK_KHR_buffer_device_address",
            "VK_KHR_bind_memory2",
            "VK_EXT_memory_priority",
            "VK_EXT_debug_utils",
            "VK_EXT_memory_budget",
            "VK_AMD_device_coherent_memory",
        };
        return EXTENSION_NAMES[static_cast<usize>(ext)];
    }

    void loadVkSupportedExtensionList() {
        core::info("Loading supported Vulkan extensions list...");
        u32 extensionCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);

        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());

        std::string extensionsMessage;
        std::unordered_map<std::string, u32> extensionVersions;
        for (const VkExtensionProperties& ext : extensions) {
            extensionsMessage += std::format("\t{:40} v{}\n", ext.extensionName, ext.specVersion);
            extensionVersions[ext.extensionName] = ext.specVersion;
        }

        core::info("Found Vulkan instance extensions:\n{}", extensionsMessage);

        for (int i = 0; i < static_cast<int>(VulkanExtension::VulkanExtensionsCount); ++i) {
            VulkanExtension ext = static_cast<VulkanExtension>(i);
            if (auto it = extensionVersions.find(getFullExtensionName(ext)); it != extensionVersions.end())
                g_supportedExtensions.versions[static_cast<usize>(ext)] = Version::fromVk(it->second);
        }
    }

    void updateVkSupportedExtensionListForDevice(PhysicalDevice& physicalDevice) {
        auto const& deviceExts = physicalDevice.extensions();
        for (int i = 0; i < static_cast<int>(VulkanExtension::VulkanExtensionsCount); ++i) {
            VulkanExtension ext = static_cast<VulkanExtension>(i);
            if (deviceExts.hasExtension(ext)) {
                if (!g_supportedExtensions.hasExtension(ext)) {
                    core::info(
                        "Found device extension {} v{}.{}.{}",
                        getFullExtensionName(ext),
                        deviceExts.getExtensionVersion(ext).major,
                        deviceExts.getExtensionVersion(ext).minor,
                        deviceExts.getExtensionVersion(ext).patch);
                    g_supportedExtensions.versions[i] = deviceExts.versions[i];
                } else if (deviceExts.getExtensionVersion(ext) < g_supportedExtensions.getExtensionVersion(ext)) {
                    core::warn(
                        "Downgraded extension {} : {}.{}.{} -> {}.{}.{}, since device supports only that version",
                        getFullExtensionName(ext),
                        g_supportedExtensions.getExtensionVersion(ext).major,
                        g_supportedExtensions.getExtensionVersion(ext).minor,
                        g_supportedExtensions.getExtensionVersion(ext).patch,
                        deviceExts.getExtensionVersion(ext).major,
                        deviceExts.getExtensionVersion(ext).minor,
                        deviceExts.getExtensionVersion(ext).patch);
                    g_supportedExtensions.versions[i] = deviceExts.versions[i];
                }
            }
        }
    }
} // namespace graphics::vulkan::internal
