#include "Queue.hpp"

namespace graphics::vulkan::internal {
    Queue::Queue(VkDevice device, uint32_t index) {
        vkGetDeviceQueue(device, index, 0, &m_queue);
    }
} // namespace graphics::vulkan::internal
