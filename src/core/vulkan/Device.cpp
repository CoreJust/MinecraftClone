#include <core/vulkan/Device.hpp>

#include <core/vulkan/Check.hpp>

#include <volk.h>

namespace core {

CORE_VK_RESOURCE_DESTROY_IMPL(RawDevice) {
    vkDestroyDevice(self.m_handle, nullptr);
}

void RawDevice::waitIdle() const {
    ASSERT(!isNull());
    std::ignore = VK_CHECK(vkDeviceWaitIdle(m_handle));
}

} // namespace core
