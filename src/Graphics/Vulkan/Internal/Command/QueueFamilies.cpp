#include "QueueFamilies.hpp"
#include <vulkan/vulkan.h>
#include <Core/Common/Assert.hpp>
#include <Core/Container/DynArray.hpp>
#include "../Vulkan/Surface.hpp"
#include "../Vulkan/PhysicalDevice.hpp"

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
        return VK_QUEUES[static_cast<usize>(type)];
    }

    QueueFamilies::QueueFamilies(PhysicalDevice const& device, Surface const& surface) : QueueFamilies() {
        u32 count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device.get(), &count, nullptr);

        core::DynArray<VkQueueFamilyProperties> queueFamilies(count);
        vkGetPhysicalDeviceQueueFamilyProperties(device.get(), &count, queueFamilies.data());

        u32 queueIndex = 0;
        for (VkQueueFamilyProperties const& props : queueFamilies) {
            for (int i = 0; i < static_cast<int>(QueueType::QueueTypesCount); ++i) {
                if (m_indices[i])
                    continue;
                QueueType type = static_cast<QueueType>(i);
                if (type == QueueType::Present) {
                    if (surface.isSupportedOn(device.get(), queueIndex)) 
                        m_indices[i] = queueIndex;
                } else if (props.queueFlags & queueTypeToVk(type)) {
                    m_indices[i] = queueIndex;
                }
            }
            ++queueIndex;
        }
    }
} // namespace graphics::vulkan::internal
