#include <core/vulkan/CommandBuffer.hpp>

#include <core/vulkan/Check.hpp>

#include <volk.h>

namespace core {

CORE_VK_RESOURCE_DESTROY_IMPL(RawCommandBuffer) {
    vkFreeCommandBuffers(device_handle, pool_handle, 1, &self.m_handle);
}

CORE_VK_RESOURCE_BATCH_DESTROY_IMPL(RawCommandBuffer) {
    vkFreeCommandBuffers(
        device_handle,
        pool_handle,
        static_cast<uint32_t>(selves.size()),
        reinterpret_cast<VkCommandBuffer*>(selves.data())
    );
}

CORE_VK_RESOURCE_DEFERRED_BATCH_CONSTRUCTION_IMPL(RawCommandBuffer,
    VkDevice const device_handle, VkCommandPool const pool_handle, CommandBufferType const type
) {
    CORE_VK_CAPTURE_DESTRUCTION_CONTEXT(){
        .device_handle = device_handle,
        .pool_handle = pool_handle,
    };

    VkCommandBufferAllocateInfo const allocate_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool_handle,
        .level = static_cast<VkCommandBufferLevel>(type),
        .commandBufferCount = static_cast<uint32_t>(selves.size()),
    };
    if (!VK_CHECK(vkAllocateCommandBuffers(device_handle, &allocate_info, reinterpret_cast<VkCommandBuffer*>(selves.data())))) {
        throw FailedToCreateCommandBufferError{ };
    }
}

void RawCommandBuffer::reset(CommandBufferReleaseResources const release_resources) {
    ASSERT(!isNull());
    if (!VK_CHECK(vkResetCommandBuffer(
        m_handle,
        static_cast<VkCommandBufferResetFlags>(VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT * bool(release_resources))
    ))) {
        throw FailedToResetCommandBufferError{ };
    }
}

void RawCommandBuffer::begin(CommandBufferUsageBits const usage) {
    ASSERT(!isNull());
    VkCommandBufferBeginInfo const begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = static_cast<VkCommandBufferUsageFlags>(usage.value),
    };
    if (!VK_CHECK(vkBeginCommandBuffer(m_handle, &begin_info))) {
        throw FailedToBeginCommandBufferError{ };
    }
}

void RawCommandBuffer::end() {
    ASSERT(!isNull());
    if (!VK_CHECK(vkEndCommandBuffer(m_handle))) {
        throw FailedToEndCommandBufferError{ };
    }
}

} // namespace core
