#pragma once
#include <Core/Container/UniquePtr.hpp>
#include "../Wrapper/Handles.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;

    class Allocator final {
        VmaAllocator m_allocator = VK_NULL_HANDLE;

    public:
        Allocator(Vulkan& vulkan);
        ~Allocator();

        VmaAllocator get() const noexcept { return m_allocator; }
    };
} // namespace graphics::vulkan::internal
