#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Memory/Function.hpp>
#include <Core/Container/ArrayView.hpp>
#include "../Wrapper/Handles.hpp"
#include "CommandPoolTypeBit.hpp"
#include "CommandBuffer.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;
    class Queue;

    class CommandPool final {
        VkCommandPool m_pool   = VK_NULL_HANDLE;
        Vulkan&       m_vulkan;
        Queue&        m_queue;
        CommandPoolTypeBit m_type;

    public:
        CommandPool(Vulkan& vulkan, CommandPoolTypeBit type, Queue& queue);
        ~CommandPool();

        void allocate(core::ArrayView<CommandBuffer> result);
        void free(core::ArrayView<CommandBuffer> result);

        void execImmediate(core::Function<void, CommandBuffer&> func);

        PURE Queue const& parentQueue() const noexcept { return m_queue; }
        PURE CommandPoolTypeBit type() const noexcept { return m_type; }
        PURE VkCommandPool get() const noexcept { return m_pool; }
    };
} // namespace graphics::vulkan::internal
