#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Memory/UniquePtr.hpp>
#include "../Command/CommandPool.hpp"
#include "../Command/CommandBuffer.hpp"
#include "../Sync/Semaphore.hpp"
#include "../Sync/Fence.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;

    class Frame final {
        CommandPool   m_pool;
        CommandBuffer m_commandBuffer;
        Semaphore     m_imageAvailable;
        Semaphore     m_renderingDone;
        Fence         m_fence;

    public:
        Frame(Vulkan& vulkan, Queue& graphicsQueue);
        ~Frame();

        PURE CommandPool        & pool()                 noexcept { return m_pool; }
        PURE CommandBuffer      & commandBuffer()        noexcept { return m_commandBuffer; }
        PURE Semaphore          & imageAvailable()       noexcept { return m_imageAvailable; }
        PURE Semaphore          & renderingDone()        noexcept { return m_renderingDone; }
        PURE Fence              & fence()                noexcept { return m_fence; }
        PURE CommandPool   const& pool()           const noexcept { return m_pool; }
        PURE CommandBuffer const& commandBuffer()  const noexcept { return m_commandBuffer; }
        PURE Semaphore     const& imageAvailable() const noexcept { return m_imageAvailable; }
        PURE Semaphore     const& renderingDone()  const noexcept { return m_renderingDone; }
        PURE Fence         const& fence()          const noexcept { return m_fence; }
    };
} // namespace graphics::vulkan::internal
