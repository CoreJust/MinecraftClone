#include "Device.hpp"
#include <Core/IO/Logger.hpp>
#include "Check.hpp"
#include "../Exception.hpp"

namespace graphics::vulkan::internal {
namespace {
    void initLogicalDevice(VkDevice& device, PhysicalDevice const& physicalDevice) {
        VkDeviceQueueCreateInfo queueCreateInfo { };
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = physicalDevice.queueFamilies().getIndex(QueueType::Graphics);
        queueCreateInfo.queueCount = 1;
        float queuePriority = 1.f;
        queueCreateInfo.pQueuePriorities = &queuePriority;

        VkPhysicalDeviceFeatures deviceFeatures { };
        VkDeviceCreateInfo createInfo { };
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pQueueCreateInfos = &queueCreateInfo;
        createInfo.queueCreateInfoCount = 1;
        createInfo.pEnabledFeatures = &deviceFeatures;

        if (!VK_CHECK(vkCreateDevice(physicalDevice.get(), &createInfo, nullptr, &device))) {
            core::io::fatal("Failed to create logical device");
            throw VulkanException { };
        }
    }
} // namespace

    Device::Device(PhysicalDevice const& physicalDevice) {
        core::io::info("Creating logical device...");
        initLogicalDevice(m_logicalDevice, physicalDevice);
    }

    Device::~Device() {
        if (m_logicalDevice != VK_NULL_HANDLE) {
            vkDestroyDevice(m_logicalDevice, nullptr);
            m_logicalDevice = VK_NULL_HANDLE;
            core::io::info("Destroyed logical device");
        }
    }
} // namespace graphics::vulkan::internal
