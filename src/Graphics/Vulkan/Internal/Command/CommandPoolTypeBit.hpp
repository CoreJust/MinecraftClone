#pragma once

namespace graphics::vulkan::internal {
    enum class CommandPoolTypeBit {
        None = 0,
        
        // Specifies that command buffers allocated from the pool will be short-lived, meaning that they 
        // will be reset or freed in a relatively short timeframe. This flag may be used by the 
        // implementation to control memory allocation behavior within the pool.
        Transient = 1,

        // Allows any command buffer allocated from a pool to be individually reset to the initial state; 
        // either by calling vkResetCommandBuffer, or via the implicit reset when calling vkBeginCommandBuffer. 
        // If this flag is not set on a pool, then vkResetCommandBuffer must not be called for any command 
        // buffer allocated from that pool.
        ResetCommandBuffer = 2,

        // Since Vulkan 1/1
        // Specifies that command buffers allocated from the pool are protected command buffers.
        Protected = 4,
    };

    constexpr CommandPoolTypeBit operator|(CommandPoolTypeBit lhs, CommandPoolTypeBit rhs) noexcept {
        return static_cast<CommandPoolTypeBit>(static_cast<unsigned int>(lhs) | static_cast<unsigned int>(rhs));
    }

    constexpr CommandPoolTypeBit operator&(CommandPoolTypeBit lhs, CommandPoolTypeBit rhs) noexcept {
        return static_cast<CommandPoolTypeBit>(static_cast<unsigned int>(lhs) & static_cast<unsigned int>(rhs));
    }
} // namespace graphics::vulkan::internal
