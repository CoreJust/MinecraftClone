#include "Queue.hpp"
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include "Check.hpp"
#include "QueueFamilies.hpp"
#include "CommandBuffer.hpp"
#include "Sync/Semaphore.hpp"
#include "Sync/Fence.hpp"
#include "../Exception.hpp"

namespace graphics::vulkan::internal {
    Queue::Queue(VkDevice device, u32 index) {
        vkGetDeviceQueue(device, index, 0, &m_queue);
    }

    void Queue::submit(CommandBuffer& commandBuffer, Semaphore& wait, Semaphore& signal, Fence& fence) {
        VkSubmitInfo submitInfo { };
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = wait.ptr();
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = commandBuffer.ptr();
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signal.ptr();

        if (!VK_CHECK(vkQueueSubmit(m_queue, 1, &submitInfo, fence.get()))) {
            core::error("Failed to submit command buffer to queue");
            throw VulkanException { };
        }
    }

    void Queue::waitIdle() const {
        if (!VK_CHECK(vkQueueWaitIdle(m_queue))) {
            core::error("Failed to wait for queue to be idle");
        }
    }
       
    QueueMaker::QueueMaker(VkDevice device, QueueFamilies const& queueFamilies) 
        : m_device(device)
        , m_queueFamilies(queueFamilies)
        , m_createdQueues(core::TypeErasedObject::make<std::unordered_map<u32, Queue*>>())
    { }
        
    core::UniquePtr<Queue> QueueMaker::make(QueueType type) {
        ASSERT(m_queueFamilies.hasFamily(type));
        auto& map = m_createdQueues.get<std::unordered_map<u32, Queue*>>();
        u32 const index = m_queueFamilies.getIndex(type);
        if (auto it = map.find(index); it != map.end())
            return core::makeUP<Queue>(*it->second);
        auto result = core::makeUP<Queue>(m_device, index);
        map[index] = result.get();
        return result;
    }
} // namespace graphics::vulkan::internal
