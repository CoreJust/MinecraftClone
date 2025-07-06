#include "Queue.hpp"
#include <unordered_map>
#include <Core/Common/Assert.hpp>
#include "QueueFamilies.hpp"

namespace graphics::vulkan::internal {
    Queue::Queue(VkDevice device, uint32_t index) {
        vkGetDeviceQueue(device, index, 0, &m_queue);
    }
       
    QueueMaker::QueueMaker(VkDevice device, QueueFamilies const& queueFamilies) 
        : m_device(device)
        , m_queueFamilies(queueFamilies)
        , m_createdQueues(core::memory::TypeErasedObject::make<std::unordered_map<uint32_t, Queue*>>())
    { }
        
    core::memory::UniquePtr<Queue> QueueMaker::make(QueueType type) {
        ASSERT(m_queueFamilies.hasFamily(type));
        auto& map = m_createdQueues.get<std::unordered_map<uint32_t, Queue*>>();
        uint32_t const index = m_queueFamilies.getIndex(type);
        if (auto it = map.find(index); it != map.end())
            return core::memory::makeUP<Queue>(*it->second);
        auto result = core::memory::makeUP<Queue>(m_device, index);
        map[index] = result.get();
        return result;
    }
} // namespace graphics::vulkan::internal
