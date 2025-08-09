#pragma once
#include <Core/Container/UniquePtr.hpp>
#include <Core/Memory/TypeErasedObject.hpp>
#include <Core/Macro/Attributes.hpp>
#include "../Wrapper/Handles.hpp"
#include "QueueType.hpp"
#include "../Pipeline/PipelineStageBit.hpp"

namespace graphics::vulkan::internal {
    class CommandBuffer;
    class Semaphore;
    class Fence;

    class Queue final {
        VkQueue m_queue = VK_NULL_HANDLE;
        u32 m_index;

    public:
        Queue() noexcept = default;
        Queue(VkDevice device, u32 index);
        Queue(Queue&&) noexcept = default;
        Queue(const Queue&) noexcept = default;
        Queue& operator=(Queue&&) &noexcept = default;
        Queue& operator=(const Queue&) &noexcept = default;

        void submit(
            CommandBuffer& commandBuffer, 
            PipelineStageBit waitStages = PipelineStageBit::None, 
            Semaphore* wait = nullptr, 
            Semaphore* signal = nullptr, 
            Fence* fence = nullptr) const;
        void waitIdle() const;

        PURE u32 index() const noexcept { return m_index; }
        PURE VkQueue get() const noexcept { return m_queue; };
    };

    class QueueFamilies;

    class QueueMaker final {
        VkDevice m_device;
        QueueFamilies const& m_queueFamilies;
        core::TypeErasedObject m_createdQueues;

    public:
        QueueMaker(VkDevice device, QueueFamilies const& queueFamilies);
        
        core::UniquePtr<Queue> make(QueueType type);
    };
} // namespace graphics::vulkan::internal

