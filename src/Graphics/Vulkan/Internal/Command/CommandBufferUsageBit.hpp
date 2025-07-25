#pragma once

namespace graphics::vulkan::internal {
    enum class CommandBufferUsageBit {
        None = 0,
        
        // Specifies that each recording of the command buffer will only be submitted once, 
        // and the command buffer will be reset and recorded again between each submission.
        OneTimeSubmit = 1,

        // Specifies that a secondary command buffer is considered to be entirely inside a 
        // render pass. If this is a primary command buffer, then this bit is ignored.
        RenderPassContinue = 2,

        // Specifies that a command buffer can be resubmitted to any queue of the same queue 
        // family while it is in the pending state, and recorded into multiple primary command 
        // buffers.
        SimultaneousUse = 4,
    };

    constexpr CommandBufferUsageBit operator|(CommandBufferUsageBit lhs, CommandBufferUsageBit rhs) noexcept {
        return static_cast<CommandBufferUsageBit>(static_cast<unsigned int>(lhs) | static_cast<unsigned int>(rhs));
    }

    constexpr CommandBufferUsageBit operator&(CommandBufferUsageBit lhs, CommandBufferUsageBit rhs) noexcept {
        return static_cast<CommandBufferUsageBit>(static_cast<unsigned int>(lhs) & static_cast<unsigned int>(rhs));
    }
} // namespace graphics::vulkan::internal
