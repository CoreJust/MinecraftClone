#pragma once

#include <core/vulkan/CommandBuffer.hpp>
#include <core/vulkan/enum/CommandPoolFlag.hpp>

namespace core::vk {

struct FailedToCreateCommandPoolError : public VulkanError {
    FailedToCreateCommandPoolError() : VulkanError{"Failed to create command pool"} { }
};

class RawCommandPool : public VulkanResourceBase<VkCommandPool> {
    CORE_VK_RESOURCE_CONTEXT(RawCommandPool,
        VkDevice device_handle;);
    // Throws FailedToCreateCommandPoolError
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(Device const& device, uint32_t const queue_family, CommandPoolFlags const flags);
};

class CommandPool final : public VulkanRaii<RawCommandPool> {
public:
    using VulkanRaii<RawCommandPool>::VulkanRaii;

    [[nodiscard]]
    CommandBuffers allocateBuffers(size_t const size, CommandBufferType const type = CommandBufferType::Primary);
};

} // namespace core::vk
