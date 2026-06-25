#pragma once

#include <core/vulkan/Device.hpp>
#include <core/vulkan/Error.hpp>

namespace core {

struct FenceCreationError : public VulkanError {
    FenceCreationError() : VulkanError("Failed to create fence") { }
};

CORE_VK_ERROR_WITH_KINDS(FenceError,
    FenceIsNull,
    FailedToWaitOnFence,
    FaiedToResetFence);

class RawFence : public VulkanResourceBase<VkFence> {
    CORE_VK_RESOURCE_CONTEXT(RawFence,
        VkDevice device_handle;);
    CORE_VK_RESOURCE_DEFER_CONSTRUCTION_FROM(Device const& device, bool const signaled);
};

class Fence : public VulkanRaii<RawFence> {
public:
    using VulkanRaii::VulkanRaii;

    // Throws FenceError
    // Returns true on successful wait
    [[nodiscard]]
    bool wait(uint64_t const timeout = static_cast<uint64_t>(-1));

    // Throws FenceError
    void reset();
};

} // namespace core
