#include "Device.hpp"
#include <vector>
#include <vulkan/vulkan.h>
#include <unordered_set>
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include "../Check.hpp"
#include "../../Exception.hpp"

namespace graphics::vulkan::internal {
namespace {
    std::vector<VkDeviceQueueCreateInfo> generateQueueCreateInfos(QueueFamilies const& queueFamilies) {
        constexpr static QueueType REQUIRED_TYPES[] = { QueueType::Graphics, QueueType::Present };
        static float const ONE = 1.f;

        std::vector<VkDeviceQueueCreateInfo> result;
        std::unordered_set<u32> addedIndices;
        for (QueueType type : REQUIRED_TYPES) {
            ASSERT(queueFamilies.hasFamily(type));
            u32 index = queueFamilies.getIndex(type);
            if (addedIndices.contains(index))
                continue;
            addedIndices.insert(index);
            result.push_back(VkDeviceQueueCreateInfo {
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .queueFamilyIndex = index,
                .queueCount = 1,
                .pQueuePriorities = &ONE,
            });
        }
        return result;
    }

    std::vector<char const*> generateDeviceExtensions(PhysicalDevice const& physicalDevice) {
        constexpr static VulkanExtension DEVICE_EXTENSIONS[] = { VulkanExtension::Swapchain };

        std::vector<char const*> result;
        for (VulkanExtension ext : DEVICE_EXTENSIONS) {
            if (physicalDevice.extensions().hasExtension(ext))
                result.emplace_back(getFullExtensionName(ext));
        }
        return result;
    }

    void initLogicalDevice(VkDevice& device, PhysicalDevice const& physicalDevice) {
        std::vector<VkDeviceQueueCreateInfo> const queueCreateInfos = generateQueueCreateInfos(physicalDevice.queueFamilies());
        std::vector<char const*> const deviceExtensions = generateDeviceExtensions(physicalDevice);

        VkPhysicalDeviceFeatures deviceFeatures { };
        VkDeviceCreateInfo createInfo { };
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.queueCreateInfoCount = static_cast<u32>(queueCreateInfos.size());
        createInfo.pEnabledFeatures = &deviceFeatures;
        createInfo.enabledExtensionCount = static_cast<u32>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();

        if (!VK_CHECK(vkCreateDevice(physicalDevice.get(), &createInfo, nullptr, &device))) {
            core::fatal("Failed to create logical device");
            throw VulkanException { };
        }
    }
} // namespace

    Device::Device(PhysicalDevice const& physicalDevice) {
        core::note("Creating logical device...");
        initLogicalDevice(m_logicalDevice, physicalDevice);
    }

    Device::~Device() {
        if (m_logicalDevice) {
            waitIdle();
            vkDestroyDevice(m_logicalDevice, nullptr);
            m_logicalDevice = VK_NULL_HANDLE;
            core::note("Destroyed logical device");
        }
    }

    
    void Device::waitIdle() const {
        vkDeviceWaitIdle(m_logicalDevice);
    }
} // namespace graphics::vulkan::internal
