#pragma once

#include <core/meta/TaggedBool.hpp>
#include <core/vulkan/Device.hpp>
#include <core/vulkan/Error.hpp>

CORE_VK_ERROR_WITH_KINDS(FenceError, VulkanRuntimeError,
    FailedToCreateFence,
    FenceIsNull,
    FailedToWaitOnFence,
    FaiedToResetFence);

namespace core::vk {

using FenceSignaled = TaggedBool<struct FenceSignaledTag>;

class RawFence : public VulkanResourceBase<VkFence> {
    CORE_VK_RESOURCE_CONTEXT(RawFence,
        VkDevice device_handle;);
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(Device const& device, FenceSignaled const signaled);
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

} // namespace core::vk
