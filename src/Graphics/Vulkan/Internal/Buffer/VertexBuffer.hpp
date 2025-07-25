#pragma once
#include "Buffer.hpp"
#include "CopyBuffer.hpp"

namespace graphics::vulkan::internal {
    class VertexBuffer final : public Buffer {
        CommandPool& m_copyCommandPool;

    public:
        VertexBuffer(Vulkan& vulkan, CommandPool& copyCommandPool, usize size)
            : Buffer(vulkan, BufferTypeBit::Vertex | BufferTypeBit::TransferDst, MemoryTypeBit::DeviceLocal, size)
            , m_copyCommandPool(copyCommandPool)
        { }

        CopyBuffer makeCopyBuffer() {
            return CopyBuffer(m_copyCommandPool, *this);
        }
    };
} // namespace graphics::vulkan::internal
