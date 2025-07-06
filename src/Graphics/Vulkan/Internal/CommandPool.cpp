#include "CommandPool.hpp"
#include <Core/IO/Logger.hpp>
#include "Check.hpp"
#include "Device.hpp"
#include "QueueFamilies.hpp"
#include "../Exception.hpp"

namespace graphics::vulkan::internal {
    CommandPool::CommandPool(Device& device, QueueFamilies const& queueFamilies) 
        : m_device(device.get()) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = queueFamilies.getIndex(QueueType::Graphics);
        poolInfo.flags = 0;

        if (!VK_CHECK(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_pool))) {
            core::io::fatal("Failed to create Vulkan command pool");
            throw VulkanException { };
        }
    }

    CommandPool::~CommandPool() {
        if (m_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_pool, nullptr);
            m_pool = VK_NULL_HANDLE;
            core::io::info("Destroyed Vulkan command pool");
        }
    }
} // namespace graphics::vulkan::internal