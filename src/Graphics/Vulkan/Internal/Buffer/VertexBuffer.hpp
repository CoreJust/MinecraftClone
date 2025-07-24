#pragma once
#include "Buffer.hpp"

namespace graphics::vulkan::internal {
    class VertexBuffer final : public Buffer {
    public:
        VertexBuffer(Vulkan& vulkan, usize size)
            : Buffer(vulkan, BufferTypeBit::Vertex, MemoryTypeBit::HostVisible | MemoryTypeBit::HostCoherent, size)
        { }
    };
} // namespace graphics::vulkan::internal
