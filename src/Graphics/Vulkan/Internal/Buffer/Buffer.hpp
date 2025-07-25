#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Common/Int.hpp>
#include "../Wrapper/Handles.hpp"
#include "BufferTypeBit.hpp"
#include "MemoryTypeBit.hpp"
#include "MappedMemory.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;
    class CommandPool;

    class Buffer {
    protected:
        VkBuffer m_buffer;
        VkDeviceMemory m_memory;
        usize m_size;
        Vulkan& m_vulkan;
        BufferTypeBit m_bufferType;
        MemoryTypeBit m_memoryType;

    public:
        Buffer(Buffer&&) noexcept;
        Buffer(Vulkan& vulkan, BufferTypeBit type, MemoryTypeBit memoryType, usize size);
        ~Buffer();

        void copyFrom(Buffer& other, CommandPool& copyCommandPool, usize size = static_cast<usize>(-1), usize otherOffset = 0, usize ownOffset = 0);
        PURE MappedMemory map(usize offset = 0, usize size = static_cast<usize>(-1));

        PURE Vulkan&       vulkan    ()       noexcept { return m_vulkan; }
        PURE VkBuffer      get       () const noexcept { return m_buffer; }
        PURE VkBuffer*     ptr       ()       noexcept { return &m_buffer; }
        PURE usize         size      () const noexcept { return m_size; }
        PURE BufferTypeBit bufferType() const noexcept { return m_bufferType; }
        PURE MemoryTypeBit memoryType() const noexcept { return m_memoryType; }
    };
} // namespace graphics::vulkan::internal
