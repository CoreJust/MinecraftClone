#include "Device.hpp"
#include <optional>
#include <vector>
#include <vulkan/vulkan.h>
#include <Core/IO/Logger.hpp>
#include "Version.hpp"
#include "Exception.hpp"
#include "Check.hpp"

namespace graphics::vulkan {
namespace {
    std::optional<uint32_t> getQueueFamilies(VkPhysicalDevice device) {
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        auto familyIt = std::find_if(queueFamilies.begin(), queueFamilies.end(), [](VkQueueFamilyProperties props) {
            return props.queueFlags & VK_QUEUE_GRAPHICS_BIT;
        });
        if (familyIt == queueFamilies.end()) 
            return std::nullopt;
        return familyIt - queueFamilies.begin();
    }

    VkPhysicalDevice choosePhysicalDevice(VkInstance instance) {
        core::io::info("Choosing physical device...");
        VkPhysicalDevice device = VK_NULL_HANDLE;
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            core::io::fatal("No physical devices found");
            throw VulkanException { };
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
        Version bestVersion { };
        for (const VkPhysicalDevice& d : devices) {
            VkPhysicalDeviceProperties deviceProperties;
            vkGetPhysicalDeviceProperties(d, &deviceProperties);
            Version apiVersion = Version::fromVk(deviceProperties.apiVersion);
            core::io::info(
                "Found physical device {} (id {})\n\tVulkan API version {}.{},{}",
                deviceProperties.deviceName,
                deviceProperties.deviceID,
                apiVersion.major, apiVersion.minor, apiVersion.patch);
            if (bestVersion < apiVersion) {
                if (auto queueFamilies = getQueueFamilies(d)) {
                    device = d;
                    bestVersion = apiVersion;
                }
            }
        }

        if (bestVersion < getVkVersion()) {
            core::io::warn(
                "Vulkan version downgraded: {}.{}.{} -> {}.{}.{}",
                getVkVersion().major, getVkVersion().minor, getVkVersion().patch,
                bestVersion.major, bestVersion.minor, bestVersion.patch);
            downgradeVkVersion(bestVersion);
        }

        if (device == VK_NULL_HANDLE) {
            core::io::fatal("No suitable physical device found");
            throw VulkanException { };
        } else {
            VkPhysicalDeviceProperties deviceProperties;
            vkGetPhysicalDeviceProperties(device, &deviceProperties);
            core::io::info("Chose device {} (id {})", deviceProperties.deviceName, deviceProperties.deviceID);
        }
        return device;
    }

    void initLogicalDevice(VkDevice& device, VkQueue& graphicsQueue, VkPhysicalDevice physicalDevice) {
        auto indices = getQueueFamilies(physicalDevice);

        VkDeviceQueueCreateInfo queueCreateInfo { };
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = *indices;
        queueCreateInfo.queueCount = 1;
        float queuePriority = 1.f;
        queueCreateInfo.pQueuePriorities = &queuePriority;

        VkPhysicalDeviceFeatures deviceFeatures { };
        VkDeviceCreateInfo createInfo { };
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pQueueCreateInfos = &queueCreateInfo;
        createInfo.queueCreateInfoCount = 1;
        createInfo.pEnabledFeatures = &deviceFeatures;

        if (!VK_CHECK(vkCreateDevice(physicalDevice, &createInfo, nullptr, &device))) {
            core::io::fatal("Failed to create logical device");
            throw VulkanException { };
        }

        vkGetDeviceQueue(device, *indices, 0, &graphicsQueue);
    }
} // namespace

    Device::Device(Instance& instance) 
        : m_physicalDevice(core::memory::TypeErasedObject::make<VkPhysicalDevice>(choosePhysicalDevice(instance.getInstance().get<VkInstance>())))
        , m_logicalDevice (core::memory::TypeErasedObject::make<VkDevice>(VK_NULL_HANDLE))
        , m_graphicsQueue(core::memory::TypeErasedObject::make<VkQueue>(VK_NULL_HANDLE))
    {
        core::io::info("Creating logical device...");
        initLogicalDevice(m_logicalDevice.get<VkDevice>(), m_graphicsQueue.get<VkQueue>(), m_physicalDevice.get<VkPhysicalDevice>());
    }

    Device::~Device() {
        if (m_logicalDevice.hasValue()) {
            vkDestroyDevice(m_logicalDevice.get<VkDevice>(), nullptr);
        }
    }
} // namespace graphics::vulkan
