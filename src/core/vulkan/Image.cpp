#include <core/vulkan/Image.hpp>

#include <core/vulkan/VMA.hpp>

namespace core {

CORE_VK_RESOURCE_DESTROY_IMPL(RawImage) {
    if (allocation.allocator != VMA_NULL && allocation.allocation != VMA_NULL) {
        vmaDestroyImage(allocation.allocator, self.m_handle, allocation.allocation);
    }
}

} // namespace core
