#include "Buffer.hpp"
#include <vulkan/vulkan.h>
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include "../Vulkan/Vulkan.hpp"
#include "../Check.hpp"

namespace graphics::vulkan::internal {

    Buffer::Buffer(Vulkan& vulkan, BufferTypeBit type, MemoryTypeBit memoryType, usize size) 
        : m_memoryType(memoryType)
        , m_size(size)
        , m_vulkan(vulkan) {
        VkBufferCreateInfo bufferInfo { };
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
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

    PURE MappedMemory Buffer::map(usize offset, usize size) {
        ASSERT(size == static_cast<usize>(-1) || (size + offset <= m_size), "Buffer::map out of buffer bounds");
        ASSERT((m_memoryType & MemoryTypeBit::HostVisible) != MemoryTypeBit::None, "Buffer is not visible to host; Cannot map it");
        return MappedMemory { m_vulkan, m_memory, offset, size == static_cast<usize>(-1) ? m_size : size };
    }
} // namespace graphics::vulkan::internal
