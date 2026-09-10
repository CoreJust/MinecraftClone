#pragma once

#include <core/common/SpanUtils.hpp>
#include <core/meta/TaggedBool.hpp>
#include <core/vulkan/Device.hpp>
#include <core/vulkan/Error.hpp>
#include <core/vulkan/enum/CommandBufferType.hpp>
#include <core/vulkan/enum/CommandBufferUsage.hpp>

CORE_VK_ERROR_WITH_KINDS(CommandBufferError, VulkanRuntimeError,
    FailedToCreateCommandBuffer,
    FailedToResetCommandBuffer,
    FailedToBeginCommandBuffer,
    FailedToEndCommandBuffer);

namespace core::vk {

using CommandBufferReleaseResources = TaggedBool<struct CommandBufferReleaseResourcesTag>;

class RawCommandBuffer : public VulkanResourceBase<VkCommandBuffer> {
    CORE_VK_RESOURCE_CONTEXT(RawCommandBuffer,
        VkDevice device_handle;
        VkCommandPool pool_handle;
        CORE_VK_BATCH_DESTROYABLE());
    // Throws CommandBufferError
    CORE_VK_RESOURCE_BATCH_CONSTRUCTION_FROM(
        VkDevice const device_handle,
        VkCommandPool const pool_handle,
        CommandBufferType const type
    );
public:
    // Throws CommandBufferError
    void reset(CommandBufferReleaseResources const release_resources = CommandBufferReleaseResources::No);
    // Throws CommandBufferError
    void begin(CommandBufferUsageBits const usage = { });
    // Throws CommandBufferError
    void end();
};

using CommandBuffer = VulkanRaii<RawCommandBuffer>;
using CommandBuffers = VulkanRaiiVector<RawCommandBuffer>;

} // namespace core::vk
