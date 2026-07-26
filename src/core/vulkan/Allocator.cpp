#include <core/vulkan/Allocator.hpp>

#include <core/common/Assert.hpp>
#include <core/meta/EnumImpl.hpp>
#include <core/vulkan/internal/VMA.hpp>

CORE_ENUM_FUNCTIONS_IMPL(::core::vk::AllocationErrorKind);

namespace core::vk {

CORE_VK_RESOURCE_DESTROY_IMPL(RawAllocator) {
    vmaDestroyAllocator(self.handle());
}

void RawAllocator::setFrameIndex(uint32_t const idx) {
    ASSERT(!isNull());
    vmaSetCurrentFrameIndex(m_handle, idx);
}

Image RawAllocator::allocImage() {
    ASSERT(false, "Not implemented");
    // TODO: implement
    return Image{ };
}

} // namespace core::vk
