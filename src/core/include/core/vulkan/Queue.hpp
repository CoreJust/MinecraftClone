#pragma once

#include <core/vulkan/Resource.hpp>
#include <core/vulkan/enum/QueueFamily.hpp>

namespace core {

class Queue final : public VulkanResourceBase<VkQueue> {
    CORE_VK_RESOURCE_CONTEXT(Queue);
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(VkQueue const queue, QueueFamily const family) {
        CORE_VK_CAPTURE_DESTRUCTION_CONTEXT(){ };
        self.m_handle = queue;
        self.m_family = family;
    }
public:
    [[nodiscard]]
    constexpr QueueFamily family() const noexcept { return m_family; }
private:
    QueueFamily m_family = QueueFamily::Count;
};

using Queues = std::array<Queue, countOf<QueueFamily>()>;

} // namespace core
