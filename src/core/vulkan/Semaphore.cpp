#include <core/vulkan/Semaphore.hpp>

#include <core/vulkan/Check.hpp>

#include <volk.h>

namespace core::vk {

CORE_VK_RESOURCE_DESTROY_IMPL(RawSemaphore) {
    vkDestroySemaphore(device_handle, self.m_handle, nullptr);
}

CORE_VK_RESOURCE_DEFERRED_CONSTRUCTION_IMPL(RawSemaphore, Device const& device) {
    CORE_VK_CAPTURE_DESTRUCTION_CONTEXT(){ .device_handle = device.handle() };

    VkSemaphoreCreateInfo const create_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    if (!VK_CHECK(vkCreateSemaphore(device.handle(), &create_info, nullptr, &self.m_handle))) {
        throw SemaphoreCreationError{ };
    }
}

} // namespace core::vk
