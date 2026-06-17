#pragma once

#include <core/vulkan/Queue.hpp>

namespace core {

class RawDevice : public VulkanResourceBase<VkDevice> {
    CORE_VK_RESOURCE_CONTEXT(RawDevice);
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(VkDevice const device, Queues const queues) {
        CORE_VK_CAPTURE_DESTRUCTION_CONTEXT(){ };
        self.m_handle = device;
        self.m_queues = queues;
    }
public:
    void waitIdle() const;

    [[nodiscard]]
    constexpr Queues const& queues() const noexcept { return m_queues; }
    [[nodiscard]]
    constexpr Queue queue(QueueFamily const family) const noexcept { return m_queues[indexOf(family)]; }
private:
    Queues m_queues;
};

using Device = VulkanRaii<RawDevice>;

} // namespace core
