#include "CommandBuffer.hpp"
#include <vulkan/vulkan.h>
#include <Core/IO/Logger.hpp>
#include <Graphics/Vulkan/Exception.hpp>
#include "../Check.hpp"

namespace graphics::vulkan::internal {
    void CommandBuffer::begin() const {
        VkCommandBufferBeginInfo beginInfo { };
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = 0;
        beginInfo.pInheritanceInfo = nullptr;

        if (!VK_CHECK(vkBeginCommandBuffer(m_buffer, &beginInfo))) {
            core::fatal("Failed to begin recording command buffers");
            throw VulkanException { };
        }
    }

    void CommandBuffer::end() const {
        if (!VK_CHECK(vkEndCommandBuffer(m_buffer))) {
            core::error("Failed to end recording command buffer");
            throw VulkanException { };
        }
    }

    void CommandBuffer::drawVertices(Buffer& buffer) {
        VkDeviceSize offsets[] = { 0 }; 
        vkCmdBindVertexBuffers(m_buffer, 0, 1, buffer.ptr(), offsets);
        vkCmdDraw(m_buffer, static_cast<uint32_t>(buffer.size()), 1, 0, 0);
    }
} // namespace graphics::vulkan::internal
