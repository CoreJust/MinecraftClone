#include "MappedMemory.hpp"
#include <vulkan/vulkan.h>
#include <Core/Memory/Exchange.hpp>
#include "../Vulkan/Vulkan.hpp"

namespace graphics::vulkan::internal {
    MappedMemory::MappedMemory(MappedMemory&& other) 
        : m_mappedMemory(core::exchange(other.m_mappedMemory, core::RawMemory { }))
        , m_memory(core::exchange(other.m_memory, VK_NULL_HANDLE))
        , m_vulkan(other.m_vulkan)
    { }
    
    MappedMemory::MappedMemory(Vulkan& vulkan, VkDeviceMemory deviceMemory, usize offset, usize size)
        : m_memory(deviceMemory)
        , m_vulkan(vulkan) {
        void* result = nullptr;
        if (!m_vulkan.safeCall<vkMapMemory>(m_memory, offset, size, 0u, &result))
            return;
        m_mappedMemory = core::RawMemory { reinterpret_cast<core::byte*>(result), size };
    }

    MappedMemory::~MappedMemory() {
        if (m_mappedMemory.data != nullptr) {
            m_vulkan.call<vkUnmapMemory>(m_memory);
            m_mappedMemory = { };
        }
    }
} // namespace graphics::vulkan::internal
