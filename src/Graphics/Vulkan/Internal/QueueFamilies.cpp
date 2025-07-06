#include "QueueFamilies.hpp"
#include <vector>
#include <Core/Common/Assert.hpp>
#include "Surface.hpp"

namespace graphics::vulkan::internal {
    VkQueueFlagBits queueTypeToVk(QueueType type) noexcept {
        ASSERT(type != QueueType::Present);
        VkQueueFlagBits VK_QUEUES[] = {
            VK_QUEUE_GRAPHICS_BIT,
            VK_QUEUE_GRAPHICS_BIT,
            VK_QUEUE_COMPUTE_BIT,
            VK_QUEUE_TRANSFER_BIT,
            VK_QUEUE_FLAG_BITS_MAX_ENUM,
        };
        return VK_QUEUES[static_cast<size_t>(type)];
    }

    QueueFamilies::QueueFamilies(VkPhysicalDevice device, Surface& surface) : QueueFamilies() {
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        uint32_t queueIndex = 0;
        for (const VkQueueFamilyProperties& props : queueFamilies) {
            for (int i = 0; i < static_cast<int>(QueueType::QueueTypesCount); ++i) {
                QueueType type = static_cast<QueueType>(i);
                if (type == QueueType::Present) {
                    if (surface.isSupportedOn(device, queueIndex))
                        m_indices[i] = queueIndex;
                } else if (props.queueFlags & queueTypeToVk(type)) {
                    m_indices[i] = queueIndex;
                }
            }
            ++queueIndex;
        }
    }
} // namespace graphics::vulkan::internal
