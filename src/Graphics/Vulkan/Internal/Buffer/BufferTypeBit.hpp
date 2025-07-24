#pragma once

namespace graphics::vulkan::internal {
    enum class BufferTypeBit {
        None         = 0,
        TransferSrc  = 1,
        TransferDst  = 2,
        UniformTexel = 4,
        StorageTexel = 8,
        Uniform      = 16,
        Storage      = 32,
        Index        = 64,
        Vertex       = 128,
        Indirect     = 256,
    };

    constexpr BufferTypeBit operator|(BufferTypeBit lhs, BufferTypeBit rhs) noexcept {
        return static_cast<BufferTypeBit>(static_cast<unsigned int>(lhs) | static_cast<unsigned int>(rhs));
    }
} // namespace graphics::vulkan::internal
