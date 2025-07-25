#include "CommandPool.hpp"
#include <vulkan/vulkan.h>
#include <Core/IO/Logger.hpp>
#include <Core/Collection/DynArray.hpp>
#include <Graphics/Vulkan/Exception.hpp>
#include "../Check.hpp"
#include "../Vulkan/Vulkan.hpp"
#include "QueueFamilies.hpp"
#include "Queue.hpp"

namespace graphics::vulkan::internal {
    CommandPool::CommandPool(Vulkan& vulkan, CommandPoolTypeBit type, Queue& queue) 
        : m_vulkan(vulkan)
        , m_queue(queue)
        , m_type(type) {
        VkCommandPoolCreateInfo poolInfo { };
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.pNext = NULL;
        poolInfo.queueFamilyIndex = queue.index();
        poolInfo.flags = static_cast<VkCommandPoolCreateFlagBits>(type);

        m_pool = m_vulkan.create<vkCreateCommandPool>(&poolInfo, nullptr);
    }

    CommandPool::~CommandPool() {
        if (m_vulkan.destroy<vkDestroyCommandPool>(m_pool, nullptr))
            core::info("Destroyed Vulkan command pool");
    }

    void CommandPool::allocate(core::ArrayView<CommandBuffer> result) {
        VkCommandBufferAllocateInfo allocInfo { };
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.pNext = NULL;
        allocInfo.commandPool = m_pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = static_cast<u32>(result.size());

        core::DynArray<VkCommandBuffer> tmp { result.size(), VK_NULL_HANDLE };
        if (!m_vulkan.safeCall<vkAllocateCommandBuffers>(&allocInfo, tmp.data())) {
            core::fatal("Failed to allocate command buffers");
            throw VulkanException { };
        }
        
        auto src = tmp.begin();
        auto const end = result.end();
        for (auto dst = result.begin(); dst < end; ++dst, ++src)
            *dst = CommandBuffer { dst->usage(), *src };
    }

    void CommandPool::free(core::ArrayView<CommandBuffer> result) {
        core::DynArray<VkCommandBuffer> tmp { result.size(), VK_NULL_HANDLE };
        m_vulkan.call<vkFreeCommandBuffers>(m_pool, static_cast<u32>(tmp.size()), tmp.data());
    }
} // namespace graphics::vulkan::internal