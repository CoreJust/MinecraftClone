#pragma once
#include "Buffer.hpp"
#include "CopyBuffer.hpp"
#include "IndexType.hpp"

namespace graphics::vulkan::internal {
    class IndexBuffer final : public Buffer {
        CommandPool& m_copyCommandPool;
        IndexType m_indexType;

    public:
        IndexBuffer(Vulkan& vulkan, CommandPool& copyCommandPool, IndexType type, usize size)
            : Buffer(vulkan, size, BufferTypeBit::Index | BufferTypeBit::TransferDst, AllocationTypeBit::None, AllocationUsage::AutoPreferDevice)
            , m_copyCommandPool(copyCommandPool)
            , m_indexType(type)
        { }

        CopyBuffer makeCopyBuffer() {
            return CopyBuffer(m_copyCommandPool, *this);
        }

        PURE u32 indexCount() const noexcept {
            switch (m_indexType) {
                case IndexType::Uint8:  return static_cast<u32>(size());
                case IndexType::Uint16: return static_cast<u32>(size() / 2);
                case IndexType::Uint32: return static_cast<u32>(size() / 4);
            }
        }

        PURE IndexType indexType() const noexcept { return m_indexType; }
    };
} // namespace graphics::vulkan::internal
