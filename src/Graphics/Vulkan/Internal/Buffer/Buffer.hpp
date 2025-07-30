#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Common/Int.hpp>
#include "../Wrapper/Handles.hpp"
#include "../Memory/AllocationTypeBit.hpp"
#include "../Memory/AllocationUsage.hpp"
#include "../Memory/MemoryTypeBit.hpp"
#include "../Memory/MappedMemory.hpp"
#include "BufferTypeBit.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;
    class CommandPool;

    class Buffer {
    protected:
        VkBuffer m_buffer;
        VmaAllocation m_allocation;
        usize m_size;
        Vulkan& m_vulkan;
        BufferTypeBit m_bufferType;
        AllocationTypeBit m_allocationType;
        AllocationUsage m_allocationUsage;

    public:
        Buffer(Buffer&&) noexcept;
        Buffer(
            Vulkan& vulkan,
            usize size,
            BufferTypeBit type,
            AllocationTypeBit allocType,
            AllocationUsage allocUsage = AllocationUsage::Auto,
            MemoryTypeBit preferredMemoryType = MemoryTypeBit::None);
        ~Buffer();

        void copyFrom(Buffer& other, CommandPool& copyCommandPool, usize size = static_cast<usize>(-1), usize otherOffset = 0, usize ownOffset = 0);
        // Note: when there is a data ready to be stored, it is best to use loadFrom(). map() is to be used when data is generated on fly
        // or non-sequentially, or there are any other specific conditions.
        PURE MappedMemory map(usize offset = 0, usize size = static_cast<usize>(-1));
        void loadFrom(core::RawMemory memory, usize offset = 0);

        PURE Vulkan&           vulkan         ()       noexcept { return m_vulkan; }
        PURE VkBuffer          get            () const noexcept { return m_buffer; }
        PURE VkBuffer*         ptr            ()       noexcept { return &m_buffer; }
        PURE usize             size           () const noexcept { return m_size; }
        PURE BufferTypeBit     bufferType     () const noexcept { return m_bufferType; }
        PURE AllocationTypeBit allocationType () const noexcept { return m_allocationType; }
        PURE AllocationUsage   allocationUsage() const noexcept { return m_allocationUsage; }
    };
} // namespace graphics::vulkan::internal
