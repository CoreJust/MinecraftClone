#pragma once
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>

namespace graphics::vulkan::internal {
    class Queue final {
        VkQueue m_queue = VK_NULL_HANDLE;

    public:
        Queue() noexcept = default;
        Queue(VkDevice device, uint32_t index);
        Queue(Queue&&) noexcept = default;
        Queue(const Queue&) noexcept = delete;
        Queue& operator=(Queue&&) noexcept = default;
        Queue& operator=(const Queue&) noexcept = delete;

        PURE VkQueue get() const noexcept { return m_queue; };
    };
} // namespace graphics::vulkan::internal

