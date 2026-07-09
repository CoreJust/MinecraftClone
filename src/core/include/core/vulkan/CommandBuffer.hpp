#pragma once

#include <core/meta/TaggedBool.hpp>
#include <core/vulkan/Device.hpp>
#include <core/vulkan/Error.hpp>
#include <core/vulkan/enum/CommandBufferType.hpp>
#include <core/vulkan/enum/CommandBufferUsage.hpp>

namespace core {

using CommandBufferReleaseResources = TaggedBool<struct CommandBufferReleaseResourcesTag>;

struct FailedToCreateCommandBufferError : public VulkanError {
    FailedToCreateCommandBufferError() : VulkanError{"Failed to create command buffer"} { }
};
struct FailedToResetCommandBufferError : public VulkanError {
    FailedToResetCommandBufferError() : VulkanError{"Failed to reset command buffer"} { }
};
struct FailedToBeginCommandBufferError : public VulkanError {
    FailedToBeginCommandBufferError() : VulkanError{"Failed to begin command buffer"} { }
};
struct FailedToEndCommandBufferError : public VulkanError {
    FailedToEndCommandBufferError() : VulkanError{"Failed to end command buffer"} { }
};

class RawCommandBuffer : public VulkanResourceBase<VkCommandBuffer> {
    CORE_VK_RESOURCE_CONTEXT(RawCommandBuffer,
        VkDevice device_handle;
        VkCommandPool pool_handle;
        CORE_VK_BATCH_DESTROYABLE());
    // Throws FailedToCreateCommandBufferError
    CORE_VK_RESOURCE_BATCH_CONSTRUCTION_FROM(
        VkDevice const device_handle,
        VkCommandPool const pool_handle,
        CommandBufferType const type
    );
public:
    // Throws FailedToResetCommandBufferError
    void reset(CommandBufferReleaseResources const release_resources = CommandBufferReleaseResources::No);
    // Throws FailedToBeginCommandBufferError
    void begin(CommandBufferUsageBits const usage = { });
    // Throws FailedToEndCommandBufferError
    void end();
};

using CommandBuffer = VulkanRaii<RawCommandBuffer>;
using CommandBuffers = VulkanRaiiVector<RawCommandBuffer>;

} // namespace core
