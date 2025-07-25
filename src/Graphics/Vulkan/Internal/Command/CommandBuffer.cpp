#include "CommandBuffer.hpp"
#include <vulkan/vulkan.h>
#include <Core/IO/Logger.hpp>
#include <Graphics/Vulkan/Exception.hpp>
#include "../Check.hpp"
#include "../Pipeline/AttributeFormat.hpp"

namespace graphics::vulkan::internal {
    void CommandBuffer::begin() const {
        VkCommandBufferBeginInfo beginInfo { };
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = static_cast<VkCommandBufferUsageFlags>(m_usage);
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
        
    void CommandBuffer::copyBuffer(VkBuffer src, VkBuffer dst, usize size, usize srcOffset, usize dstOffset) {
        VkBufferCopy region {
            .srcOffset = srcOffset,
            .dstOffset = dstOffset,
            .size      = size,
        };
        vkCmdCopyBuffer(m_buffer, src, dst, 1, &region);
    }
        
    void CommandBuffer::pushConstants(VkPipelineLayout pipelineLayout, ShaderStageBit stage, core::RawMemory constants) {
        vkCmdPushConstants(m_buffer, pipelineLayout, static_cast<VkShaderStageFlagBits>(stage), 0, static_cast<u32>(constants.size), constants.data);
    }

    void CommandBuffer::drawVertices(VertexBuffer& buffer) {
        VkDeviceSize offsets[] = { 0 }; 
        vkCmdBindVertexBuffers(m_buffer, 0, 1, buffer.ptr(), offsets);
        vkCmdDraw(m_buffer, static_cast<uint32_t>(buffer.size()), 1, 0, 0);
    }
} // namespace graphics::vulkan::internal
