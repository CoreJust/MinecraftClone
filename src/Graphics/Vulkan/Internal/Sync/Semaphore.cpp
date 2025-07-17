#include "Semaphore.hpp"
#include <vulkan/vulkan.h>
#include "../Vulkan/Vulkan.hpp"

namespace graphics::vulkan::internal {
    Semaphore::Semaphore(Vulkan& vulkan) 
        : m_vulkan(vulkan) {
        VkSemaphoreCreateInfo semaphoreInfo { };
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        m_semaphore = m_vulkan.create<vkCreateSemaphore>(&semaphoreInfo, nullptr);
    }

    Semaphore::~Semaphore() {
        m_vulkan.destroy<vkDestroySemaphore>(m_semaphore, nullptr);
    }
} // namespace graphics::vulkan::internal
