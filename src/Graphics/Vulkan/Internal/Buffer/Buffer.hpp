#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Common/Int.hpp>
#include "../Wrapper/Handles.hpp"
#include "BufferTypeBit.hpp"
#include "MemoryTypeBit.hpp"
#include "MappedMemory.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;

    class Buffer {
        VkBuffer m_buffer;
        VkDeviceMemory m_memory;
        MemoryTypeBit m_memoryType;
        usize m_size;
        Vulkan&  m_vulkan;

    public:
        Buffer(Vulkan& vulkan, BufferTypeBit type, MemoryTypeBit memoryType, usize size);
        ~Buffer();

        PURE MappedMemory map(usize offset = 0, usize size = static_cast<usize>(-1));
        PURE VkBuffer  get () const noexcept { return m_buffer; }
        PURE VkBuffer* ptr ()       noexcept { return &m_buffer; }
        PURE usize     size() const noexcept { return m_size; }
    };
} // namespace graphics::vulkan::internal
