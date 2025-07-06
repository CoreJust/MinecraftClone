#include "Extensions.hpp"
#include <string>
#include <format>
#include <vector>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <Core/IO/Logger.hpp>
#include "Internal/PhysicalDevice.hpp"

namespace graphics::vulkan {
    SupportedExtensions g_supportedExtensions { };
        
    SupportedExtensions::SupportedExtensions() noexcept {
        memset(versions, 255, std::size(versions));
    }
    
    SupportedExtensions::SupportedExtensions(void* physicalDevice) : SupportedExtensions() {
        internal::PhysicalDevice& device = *reinterpret_cast<internal::PhysicalDevice*>(physicalDevice);
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(device.get(), nullptr, &extensionCount, nullptr);

        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device.get(), nullptr, &extensionCount, extensions.data());

        std::unordered_map<std::string, uint32_t> extensionVersions;
        for (const VkExtensionProperties& ext : extensions)
            extensionVersions[ext.extensionName] = ext.specVersion;

        for (int i = 0; i < static_cast<int>(VulkanExtension::VulkanExtensionsCount); ++i) {
            VulkanExtension ext = static_cast<VulkanExtension>(i);
            if (auto it = extensionVersions.find(getFullExtensionName(ext)); it != extensionVersions.end())
                versions[static_cast<size_t>(ext)] = Version::fromVk(it->second);
        }
    }

    bool SupportedExtensions::hasExtension(VulkanExtension ext) const noexcept {
        return getExtensionVersion(ext).variant != UINT32_MAX;
    }

    Version SupportedExtensions::getExtensionVersion(VulkanExtension ext) const noexcept {
        return versions[static_cast<size_t>(ext)];
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

        core::io::info("Found Vulkan instance extensions:\n{}", extensionsMessage);

        for (int i = 0; i < static_cast<int>(VulkanExtension::VulkanExtensionsCount); ++i) {
            VulkanExtension ext = static_cast<VulkanExtension>(i);
            if (auto it = extensionVersions.find(getFullExtensionName(ext)); it != extensionVersions.end())
                g_supportedExtensions.versions[static_cast<size_t>(ext)] = Version::fromVk(it->second);
        }
    }

    void updateVkSupportedExtensionListForDevice(void* physicalDevice) {
        internal::PhysicalDevice& device = *reinterpret_cast<internal::PhysicalDevice*>(physicalDevice);
        auto const& deviceExts = device.extensions();
        for (int i = 0; i < static_cast<int>(VulkanExtension::VulkanExtensionsCount); ++i) {
            VulkanExtension ext = static_cast<VulkanExtension>(i);
            if (deviceExts.hasExtension(ext)) {
                if (!g_supportedExtensions.hasExtension(ext)) {
                    core::io::info(
                        "Found device extension {} v{}.{}.{}",
                        getFullExtensionName(ext),
                        deviceExts.getExtensionVersion(ext).major,
                        deviceExts.getExtensionVersion(ext).minor,
                        deviceExts.getExtensionVersion(ext).patch);
                    g_supportedExtensions.versions[i] = deviceExts.versions[i];
                } else if (g_supportedExtensions.getExtensionVersion(ext) < deviceExts.getExtensionVersion(ext)) {
                    core::io::warn(
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
} // namespace graphics::vulkan
