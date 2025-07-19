#include "Buffer.hpp"
#include <vulkan/vulkan.h>
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include "../Vulkan/Vulkan.hpp"
#include "../Check.hpp"

namespace graphics::vulkan::internal {
namespace {
    VkBufferUsageFlagBits makeBufferUsage(BufferType type) noexcept {
        constexpr static VkBufferUsageFlagBits USAGES[] = { VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_BUFFER_USAGE_INDEX_BUFFER_BIT };
        ASSERT(static_cast<usize>(type) < static_cast<usize>(BufferType::BufferTypesCount));
        return USAGES[static_cast<usize>(type)];
    }
} // namespace

    Buffer::Buffer(Vulkan& vulkan, BufferType type, usize size) 
        : m_size(size)
        , m_vulkan(vulkan) {
        VkBufferCreateInfo bufferInfo { };
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = makeBufferUsage(type);
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        m_buffer = m_vulkan.create<vkCreateBuffer>(&bufferInfo, nullptr);

        VkMemoryRequirements memRequirements;
        m_vulkan.call<vkGetBufferMemoryRequirements>(m_buffer, &memRequirements);
        m_memory = m_vulkan.allocDevice(memRequirements.size, memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        m_vulkan.safeCall<vkBindBufferMemory>(m_buffer, m_memory, 0ull);
    }

    Buffer::~Buffer() {
        m_vulkan.device().waitIdle();
        m_vulkan.destroy<vkDestroyBuffer>(m_buffer, nullptr);
        m_vulkan.destroy<vkFreeMemory>(m_memory, nullptr);
    }

    PURE MappedMemory Buffer::map(usize offset, usize size) {
        ASSERT(size == static_cast<usize>(-1) || (size + offset <= m_size), "Buffer::map out of buffer bounds");
        return MappedMemory { m_vulkan, m_memory, offset, size == static_cast<usize>(-1) ? m_size : size };
    }
} // namespace graphics::vulkan::internal
