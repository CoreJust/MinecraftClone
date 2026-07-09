#pragma once

#include <core/vulkan/Error.hpp>
#include <core/vulkan/Image.hpp>
#include <core/vulkan/Resource.hpp>

CORE_VK_ERROR_WITH_KINDS(AllocationError, VulkanRuntimeError,
    FailedToAllocateImage);

namespace core::vk {

class RawAllocator : public VulkanResourceBase<VmaAllocator> {
    CORE_VK_RESOURCE_CONTEXT(RawAllocator);
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(VmaAllocator const allocator) {
        CORE_VK_CAPTURE_DESTRUCTION_CONTEXT(){ };
        self.m_handle = allocator;
    }
public:
    void setFrameIndex(uint32_t const idx);

    // Throws FailedToAllocateImage
    [[nodiscard]]
    Image allocImage();
};

using Allocator = VulkanRaii<RawAllocator>;

} // namespace core::vk
