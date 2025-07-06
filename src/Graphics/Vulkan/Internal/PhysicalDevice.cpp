#include "PhysicalDevice.hpp"
#include <Core/IO/Logger.hpp>
#include "../Version.hpp"
#include "../Exception.hpp"

namespace graphics::vulkan::internal {
    PhysicalDevice::PhysicalDevice(VkPhysicalDevice device) noexcept
        : m_physicalDevice(device) {
        vkGetPhysicalDeviceProperties(m_physicalDevice, &m_properties);
        m_queueFamilies = QueueFamilies(m_physicalDevice);
    }

    std::vector<PhysicalDevice> PhysicalDevice::getPhysicalDevices(Instance& instance) noexcept {
        core::io::info("Choosing physical device...");
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance.get(), &deviceCount, nullptr);
        if (deviceCount == 0)
            return { };

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance.get(), &deviceCount, devices.data());

        std::vector<PhysicalDevice> result;
        result.reserve(deviceCount);
        for (const VkPhysicalDevice& d : devices)
            result.push_back(PhysicalDevice { d });
        return result;
    }

    PhysicalDevice PhysicalDevice::choosePhysicalDevice(Instance& instance) {
        core::io::info("Choosing physical device...");
        std::vector<PhysicalDevice> devices = getPhysicalDevices(instance);
        PhysicalDevice* result = nullptr;
        if (devices.empty()) {
            core::io::fatal("No physical devices found");
            throw VulkanException { };
        }

        Version bestVersion { };
        for (PhysicalDevice& d : devices) {
            Version apiVersion = Version::fromVk(d.props().apiVersion);
            core::io::info(
                "Found physical device {} (id {})\n\tVulkan API version {}.{}.{}",
                d.props().deviceName,
                d.props().deviceID,
                apiVersion.major, apiVersion.minor, apiVersion.patch);
            if (d.queueFamilies().hasFamily(QueueType::Graphics) && bestVersion < apiVersion) {
                result = &d;
                bestVersion = apiVersion;
            }
        }

        if (bestVersion < getVkVersion()) {
            core::io::warn(
                "Vulkan version downgraded: {}.{}.{} -> {}.{}.{}",
                getVkVersion().major, getVkVersion().minor, getVkVersion().patch,
                bestVersion.major, bestVersion.minor, bestVersion.patch);
            downgradeVkVersion(bestVersion);
        }

        if (!result) {
            core::io::fatal("No suitable physical device found");
            throw VulkanException { };
        } else {
            core::io::info("Chose device {} (id {})", result->props().deviceName, result->props().deviceID);
        }
        return std::move(*result);
    }
} // namespace graphics::vulkan::internal
