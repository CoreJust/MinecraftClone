#include <core/vulkan/Allocator.hpp>

#include <core/common/Assert.hpp>
#include <core/meta/EnumImpl.hpp>
#include <core/vulkan/VMA.hpp>

namespace core {

CORE_ENUM_FUNCTIONS_IMPL(AllocationErrorKind);

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

} // namespace core
