#include "CommandBuffer.hpp"
#include <Core/IO/Logger.hpp>
#include "Check.hpp"
#include "../Exception.hpp"

namespace graphics::vulkan::internal {
    void CommandBuffer::begin() const {
        VkCommandBufferBeginInfo beginInfo { };
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = 0;
        beginInfo.pInheritanceInfo = nullptr;

        if (!VK_CHECK(vkBeginCommandBuffer(m_buffer, &beginInfo))) {
            core::io::fatal("Failed to begin recording command buffers");
            throw VulkanException { };
        }
    }

    void CommandBuffer::end() const {
        if (!VK_CHECK(vkEndCommandBuffer(m_buffer))) {
            core::io::error("Failed to end recording command buffer");
            throw VulkanException { };
        }
    }
} // namespace graphics::vulkan::internal
