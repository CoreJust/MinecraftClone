#include <core/vulkan/ImageView.hpp>

#include <volk.h>

namespace core {

CORE_VK_RESOURCE_DESTROY_IMPL(RawImageView) {
    vkDestroyImageView(device.handle(), self.m_handle, nullptr);
}

} // namespace core
