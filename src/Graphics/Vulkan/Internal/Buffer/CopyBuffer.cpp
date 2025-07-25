#include "CopyBuffer.hpp"
#include "../Command/CommandPool.hpp"

namespace graphics::vulkan::internal {
    CopyBuffer::CopyBuffer(CommandPool& copyCommandPool, Buffer& destination)
        : Buffer(destination.vulkan(), BufferTypeBit::TransferSrc, MemoryTypeBit::HostCoherent | MemoryTypeBit::HostVisible, destination.size())
        , m_destination(destination)
        , m_copyCommandPool(copyCommandPool)
    {
        ASSERT((destination.bufferType() & BufferTypeBit::TransferDst) != BufferTypeBit::None, "Can only create copy buffer for destination with TransferDst type");
    }

    CopyBuffer::~CopyBuffer() {
        if (m_buffer != VK_NULL_HANDLE)
            m_destination.copyFrom(*this, m_copyCommandPool, m_size, 0, 0);
    }
} // namespace graphics::vulkan::internal
