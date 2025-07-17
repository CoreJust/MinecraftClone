#include "Fence.hpp"
#include <vulkan/vulkan.h>
#include "../Vulkan/Vulkan.hpp"

namespace graphics::vulkan::internal {
    Fence::Fence(Vulkan& vulkan) 
        : m_vulkan(vulkan) {
        VkFenceCreateInfo fenceInfo { };
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        m_fence = m_vulkan.create<vkCreateFence>(&fenceInfo, nullptr);
    }

    void Fence::wait() {
        m_vulkan.safeCall<vkWaitForFences>(1u, &m_fence, VK_TRUE, UINT64_MAX);
    }

    void Fence::reset() {
        m_vulkan.safeCall<vkResetFences>(1u, &m_fence);
    }

    Fence::~Fence() {
        if (m_fence) {
            m_vulkan.destroy<vkDestroyFence>(m_fence, nullptr);
            m_fence = VK_NULL_HANDLE;
        }
    }
} // namespace graphics::vulkan::internal
