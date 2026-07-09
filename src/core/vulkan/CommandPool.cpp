#include <core/vulkan/CommandPool.hpp>

#include <core/vulkan/Check.hpp>

#include <volk.h>

namespace core::vk {

CORE_VK_RESOURCE_DESTROY_IMPL(RawCommandPool) {
    vkDestroyCommandPool(device_handle, self.m_handle, nullptr);
}

CORE_VK_RESOURCE_DEFERRED_CONSTRUCTION_IMPL(RawCommandPool,
    Device const& device, uint32_t const queue_family, CommandPoolFlags const flags
) {
    CORE_VK_CAPTURE_DESTRUCTION_CONTEXT(){ .device_handle = device.handle() };

    VkCommandPoolCreateInfo const allocate_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = static_cast<VkCommandPoolCreateFlags>(flags.value),
        .queueFamilyIndex = queue_family,
    };
    if (!VK_CHECK(vkCreateCommandPool(device.handle(), &allocate_info, nullptr, &self.m_handle))) {
        throw FailedToCreateCommandPoolError{ };
    }
}

CommandBuffers CommandPool::allocateBuffers(size_t const size, CommandBufferType const type) {
    return CommandBuffers(size, destroyer().device_handle, handle(), type);
}

} // namespace core::vk
