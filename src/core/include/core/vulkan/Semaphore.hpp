#pragma once

#include <core/vulkan/Device.hpp>
#include <core/vulkan/Error.hpp>

namespace core {

struct SemaphoreCreationError : public VulkanError {
    SemaphoreCreationError() : VulkanError("Failed to create semaphore") { }
};

class RawSemaphore : public VulkanResourceBase<VkSemaphore> {
    CORE_VK_RESOURCE_CONTEXT(RawSemaphore,
        VkDevice device_handle;);
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(Device const& device);
};

using Semaphore = VulkanRaii<RawSemaphore>;

} // namespace core
