#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Memory/RawMemory.hpp>
#include "../Wrapper/Handles.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;

    class MappedMemory final {
        core::RawMemory m_mappedMemory { };
        VmaAllocation   m_allocation;
        Vulkan&         m_vulkan;

    public:
        MappedMemory(MappedMemory&& other);
        MappedMemory(Vulkan& vulkan, VmaAllocation allocation, usize offset, usize size);
        ~MappedMemory();

        PURE core::RawMemory get() const noexcept { return m_mappedMemory; }
    };
} // namespace graphics::vulkan::internal
