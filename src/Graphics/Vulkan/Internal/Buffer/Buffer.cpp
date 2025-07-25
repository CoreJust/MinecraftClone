#include "Buffer.hpp"
#include <vulkan/vulkan.h>
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include "../Command/CommandPool.hpp"
#include "../Command/CommandBuffer.hpp"
#include "../Command/Queue.hpp"
#include "../Vulkan/Vulkan.hpp"
#include "../Check.hpp"

namespace graphics::vulkan::internal {
    Buffer::Buffer(Buffer&& other) noexcept
        : m_buffer(core::exchange(other.m_buffer, VK_NULL_HANDLE))
        , m_memory(core::exchange(other.m_memory, VK_NULL_HANDLE))
        , m_size(core::exchange(other.m_size, 0ull))
        , m_vulkan(other.m_vulkan)
        , m_bufferType(other.m_bufferType)
        , m_memoryType(other.m_memoryType)
    { }

    Buffer::Buffer(Vulkan& vulkan, BufferTypeBit type, MemoryTypeBit memoryType, usize size) 
        : m_size(size)
        , m_vulkan(vulkan)
        , m_bufferType(type)
        , m_memoryType(memoryType)
    {
        VkBufferCreateInfo bufferInfo { };
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size  = size;
        bufferInfo.usage = static_cast<VkBufferUsageFlags>(type);
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        m_buffer = m_vulkan.create<vkCreateBuffer>(&bufferInfo, nullptr);

        VkMemoryRequirements memRequirements;
        m_vulkan.call<vkGetBufferMemoryRequirements>(m_buffer, &memRequirements);
        m_memory = m_vulkan.allocDevice(memRequirements.size, memRequirements.memoryTypeBits, static_cast<VkMemoryPropertyFlagBits>(memoryType));
        m_vulkan.safeCall<vkBindBufferMemory>(m_buffer, m_memory, 0ull);
    }

    Buffer::~Buffer() {
        m_vulkan.device().waitIdle();
        m_vulkan.destroy<vkDestroyBuffer>(m_buffer, nullptr);
        m_vulkan.destroy<vkFreeMemory>(m_memory, nullptr);
    }

    void Buffer::copyFrom(Buffer& other, CommandPool& copyCommandPool, usize size, usize otherOffset, usize ownOffset) {
        ASSERT((copyCommandPool.type() & CommandPoolTypeBit::Transient) != CommandPoolTypeBit::None,
            "To copy a buffer, a command bool for transient command buffers must be provided.");
        if (size == static_cast<usize>(-1))
            size = m_size;
        ASSERT(otherOffset + size <= other.size(), "Cannot copy buffer: region in source buffer out of buffer bounds");
        ASSERT(ownOffset   + size <= m_size,       "Cannot copy buffer: region in destination buffer out of buffer bounds");
        CommandBuffer copyBuffer { CommandBufferUsageBit::OneTimeSubmit };
        copyCommandPool.allocate({ &copyBuffer, 1 });
        copyBuffer.begin();
        copyBuffer.copyBuffer(other.get(), m_buffer, size, otherOffset, ownOffset);
        copyBuffer.end();
        copyCommandPool.parentQueue().submit(copyBuffer);
        copyCommandPool.parentQueue().waitIdle();
        copyCommandPool.free({ &copyBuffer, 1 });
    }

    PURE MappedMemory Buffer::map(usize offset, usize size) {
        ASSERT(size == static_cast<usize>(-1) || (size + offset <= m_size), "Buffer::map out of buffer bounds");
        ASSERT((m_memoryType & MemoryTypeBit::HostVisible) != MemoryTypeBit::None, "Buffer is not visible to host; Cannot map it");
        return MappedMemory { m_vulkan, m_memory, offset, size == static_cast<usize>(-1) ? m_size : size };
    }
} // namespace graphics::vulkan::internal
