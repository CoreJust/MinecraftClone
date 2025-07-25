#pragma once
#include "Buffer.hpp"

namespace graphics::vulkan::internal {
    class CopyBuffer final : public Buffer {
        Buffer& m_destination;
        CommandPool& m_copyCommandPool;

    public:
        CopyBuffer(CopyBuffer&&) noexcept = default;
        CopyBuffer(CommandPool& copyCommandPool, Buffer& destination);
        ~CopyBuffer();
    };
} // namespace graphics::vulkan::internal
