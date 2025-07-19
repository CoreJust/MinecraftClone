#pragma once
#include <Core/Macro/Attributes.hpp>
#include "../Wrapper/Handles.hpp"
#include "../Buffer/Buffer.hpp"

namespace graphics::vulkan::internal {
    class CommandBuffer final {
        VkCommandBuffer m_buffer;

    public:
        CommandBuffer(VkCommandBuffer buffer = VK_NULL_HANDLE) noexcept : m_buffer(buffer) { }

        void begin() const;
        void end() const;
        void drawVertices(Buffer& buffer);

        PURE VkCommandBuffer  get() const noexcept { return m_buffer; }
        PURE VkCommandBuffer* ptr()       noexcept { return &m_buffer; }
    };
} // namespace graphics::vulkan::internal
