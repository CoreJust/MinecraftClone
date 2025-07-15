#pragma once
#include <vulkan/vulkan.h>
#include <Core/Memory/UniquePtr.hpp>
#include <Core/Memory/TypeErasedObject.hpp>
#include <Core/Macro/Attributes.hpp>
#include "QueueType.hpp"

namespace graphics::vulkan::internal {
    class CommandBuffer;
    class Semaphore;

    class Queue final {
        VkQueue m_queue = VK_NULL_HANDLE;

    public:
        Queue() noexcept = default;
        Queue(VkDevice device, u32 index);
        Queue(Queue&&) noexcept = default;
        Queue(const Queue&) noexcept = default;
        Queue& operator=(Queue&&) noexcept = default;
        Queue& operator=(const Queue&) noexcept = default;

        void submit(CommandBuffer& commandBuffer, Semaphore& wait, Semaphore& signal);
        void waitIdle() const;

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

