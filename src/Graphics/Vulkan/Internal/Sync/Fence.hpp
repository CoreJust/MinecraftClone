#pragma once
#include <Core/Macro/Attributes.hpp>
#include "../Wrapper/Handles.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;

    class Fence final {
        VkFence m_fence = VK_NULL_HANDLE;
        Vulkan& m_vulkan;

    public:
        Fence(Vulkan& vulkan);
        ~Fence();

        void wait();
        void reset();

        PURE VkFence  get() const noexcept { return m_fence; }
        PURE VkFence* ptr()       noexcept { return &m_fence; }
    };
} // namespace graphics::vulkan::internal
