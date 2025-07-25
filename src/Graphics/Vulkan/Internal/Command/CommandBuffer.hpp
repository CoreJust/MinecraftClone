#pragma once
#include <Core/Macro/Attributes.hpp>
#include "../Wrapper/Handles.hpp"
#include "../Buffer/VertexBuffer.hpp"
#include "../Buffer/IndexBuffer.hpp"
#include "../Pipeline/ShaderStageBit.hpp"
#include "CommandBufferUsageBit.hpp"

namespace graphics::vulkan::internal {
    class CommandBuffer final {
        VkCommandBuffer m_buffer;
        CommandBufferUsageBit m_usage;

    public:
        CommandBuffer(CommandBufferUsageBit usage = CommandBufferUsageBit::None, VkCommandBuffer buffer = VK_NULL_HANDLE) noexcept 
            : m_buffer(buffer)
            , m_usage(usage)
        { }

        void begin() const;
        void end() const;
        void copyBuffer(VkBuffer src, VkBuffer dst, usize size, usize srcOffset, usize dstOffset);
        void pushConstants(VkPipelineLayout pipelineLayout, ShaderStageBit stage, core::RawMemory constants);
        void drawVertices(VertexBuffer& vertices, IndexBuffer* indices = nullptr);

        PURE CommandBufferUsageBit usage() const noexcept { return m_usage; }
        PURE VkCommandBuffer  get() const noexcept { return m_buffer; }
        PURE VkCommandBuffer* ptr()       noexcept { return &m_buffer; }
    };
} // namespace graphics::vulkan::internal
