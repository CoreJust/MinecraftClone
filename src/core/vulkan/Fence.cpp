#include <core/vulkan/Fence.hpp>

#include <core/meta/EnumImpl.hpp>
#include <core/vulkan/Check.hpp>

#include <volk.h>

CORE_ENUM_FUNCTIONS_IMPL(vk::FenceErrorKind);

namespace core::vk {

CORE_VK_RESOURCE_DESTROY_IMPL(RawFence) {
    vkDestroyFence(device_handle, self.m_handle, nullptr);
}

CORE_VK_RESOURCE_DEFERRED_CONSTRUCTION_IMPL(RawFence, Device const& device, FenceSignaled const signaled) {
    CORE_VK_CAPTURE_DESTRUCTION_CONTEXT(){ .device_handle = device.handle() };

    VkFenceCreateInfo const create_info{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = static_cast<VkFenceCreateFlags>(signaled.value * VK_FENCE_CREATE_SIGNALED_BIT),
    };
    if (!VK_CHECK(vkCreateFence(device.handle(), &create_info, nullptr, &self.m_handle))) {
        throw FenceError{ FenceError::FailedToCreateFence };
    }
}

bool Fence::wait(uint64_t const timeout) {
    if (isNull()) {
        throw FenceError(FenceError::FenceIsNull);
    }

    VkResult const result = vkWaitForFences(destroyer().device_handle, 1, handlePtr(), VK_TRUE, timeout);
    if (!VK_CHECK(result)) {
        throw FenceError(FenceError::FailedToWaitOnFence);
    }
    return result == VK_SUCCESS;
}

void Fence::reset() {
    if (isNull()) {
        throw FenceError(FenceError::FenceIsNull);
    }
    if (!VK_CHECK(vkResetFences(destroyer().device_handle, 1, handlePtr()))) {
        throw FenceError(FenceError::FaiedToResetFence);
    }
}

} // namespace core::vk
