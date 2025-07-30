#include "Buffer.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include "../Command/CommandPool.hpp"
#include "../Command/CommandBuffer.hpp"
#include "../Command/Queue.hpp"
#include "../Vulkan/Vulkan.hpp"
#include "../Check.hpp"

namespace graphics::vulkan::internal {
    Buffer::Buffer(Buffer&& other) noexcept
        : m_buffer         (core::exchange(other.m_buffer, VK_NULL_HANDLE))
        , m_allocation     (core::exchange(other.m_allocation, VK_NULL_HANDLE))
        , m_size           (core::exchange(other.m_size, 0ull))
        , m_vulkan         (other.m_vulkan)
        , m_bufferType     (other.m_bufferType)
        , m_allocationType (other.m_allocationType)
        , m_allocationUsage(other.m_allocationUsage)
    { }

    Buffer::Buffer(Vulkan& vulkan, usize size, BufferTypeBit type, AllocationTypeBit allocType, AllocationUsage allocUsage, MemoryTypeBit preferredMemoryType) 
        : m_size           (size)
        , m_vulkan         (vulkan)
        , m_bufferType     (type)
        , m_allocationType (allocType)
        , m_allocationUsage(allocUsage)
    {
        VkBufferCreateInfo bufferInfo { };
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size  = size;
        bufferInfo.usage = static_cast<VkBufferUsageFlags>(type);

        VmaAllocationCreateInfo allocInfo { };
        allocInfo.usage = static_cast<VmaMemoryUsage>(m_allocationUsage);
        allocInfo.flags = static_cast<VmaAllocationCreateFlags>(allocType);
        allocInfo.preferredFlags = static_cast<VkMemoryPropertyFlags>(preferredMemoryType);

        m_vulkan.safeCall<vmaCreateBuffer>(&bufferInfo, &allocInfo, &m_buffer, &m_allocation, nullptr);
    }

    Buffer::~Buffer() {
        m_vulkan.device().waitIdle();
        m_vulkan.destroy<vmaDestroyBuffer>(m_buffer, m_allocation);
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
        ASSERT((m_allocationType & AllocationTypeBit::CanBeMapped) != AllocationTypeBit::None, "Buffer is not visible to host; Cannot map it");
        return MappedMemory { m_vulkan, m_allocation, offset, size == static_cast<usize>(-1) ? m_size - offset : size };
    }
    
    void Buffer::loadFrom(core::RawMemory memory, usize offset) {
        ASSERT(memory.size + offset <= m_size, "Buffer::map out of buffer bounds");
        ASSERT((m_allocationType & AllocationTypeBit::CanBeMapped) != AllocationTypeBit::None, "Buffer is not visible to host; Cannot map it");
        m_vulkan.safeCall<vmaCopyMemoryToAllocation>(memory.data, m_allocation, offset, memory.size);
    }
} // namespace graphics::vulkan::internal
