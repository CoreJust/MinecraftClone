#pragma once
#include <Core/Macro/Attributes.hpp>
#include "../Wrapper/Handles.hpp"

namespace graphics::vulkan::internal {
    class CommandBuffer final {
        VkCommandBuffer m_buffer;

    public:
        CommandBuffer(VkCommandBuffer buffer = VK_NULL_HANDLE) noexcept : m_buffer(buffer) { }

        void begin() const;
        void end() const;

        PURE VkCommandBuffer  get() const noexcept { return m_buffer; }
        PURE VkCommandBuffer* ptr()       noexcept { return &m_buffer; }
    };
} // namespace graphics::vulkan::internal
