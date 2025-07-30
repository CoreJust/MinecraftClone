#include "MappedMemory.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <Core/Memory/Exchange.hpp>
#include "../Vulkan/Vulkan.hpp"

namespace graphics::vulkan::internal {
    MappedMemory::MappedMemory(MappedMemory&& other) 
        : m_mappedMemory(core::exchange(other.m_mappedMemory, core::RawMemory { }))
        , m_allocation(core::exchange(other.m_allocation, VK_NULL_HANDLE))
        , m_vulkan(other.m_vulkan)
    { }
    
    MappedMemory::MappedMemory(Vulkan& vulkan, VmaAllocation allocation, usize offset, usize size)
        : m_allocation(allocation)
        , m_vulkan(vulkan) {
        void* result = nullptr;
        if (!m_vulkan.safeCall<vmaMapMemory>(m_allocation, &result))
            return;
        m_mappedMemory = core::RawMemory { reinterpret_cast<core::byte*>(result) + offset, size };
    }

    MappedMemory::~MappedMemory() {
        if (m_mappedMemory.data != nullptr) {
            m_vulkan.call<vmaUnmapMemory>(m_allocation);
            m_mappedMemory = { };
        }
    }
} // namespace graphics::vulkan::internal
