#include "CommandPool.hpp"
#include <Core/IO/Logger.hpp>
#include <Core/Collection/DynArray.hpp>
#include "Check.hpp"
#include "Vulkan/Vulkan.hpp"
#include "QueueFamilies.hpp"
#include "../Exception.hpp"

namespace graphics::vulkan::internal {
    CommandPool::CommandPool(Vulkan& vulkan, u32 queueIndex) 
        : m_vulkan(vulkan) {
        VkCommandPoolCreateInfo poolInfo { };
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.pNext = NULL;
        poolInfo.queueFamilyIndex = queueIndex;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        m_pool = m_vulkan.create<vkCreateCommandPool>(&poolInfo, nullptr);
    }

    CommandPool::~CommandPool() {
        if (m_vulkan.destroy<vkDestroyCommandPool>(m_pool, nullptr))
            core::io::info("Destroyed Vulkan command pool");
    }

    void CommandPool::allocate(core::collection::ArrayView<CommandBuffer> result) {
        VkCommandBufferAllocateInfo allocInfo { };
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.pNext = NULL;
        allocInfo.commandPool = m_pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = static_cast<u32>(result.size());

        core::collection::DynArray<VkCommandBuffer> tmp { result.size(), VK_NULL_HANDLE };
        if (!m_vulkan.safeCall<vkAllocateCommandBuffers>(&allocInfo, tmp.data())) {
            core::io::fatal("Failed to allocate command buffers");
            throw VulkanException { };
        }
        
        auto src = tmp.begin();
        auto const end = result.end();
        for (auto dst = result.begin(); dst < end; ++dst, ++src)
            *dst = CommandBuffer { *src };
    }
} // namespace graphics::vulkan::internal