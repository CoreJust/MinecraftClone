#include <core/vulkan/Image.hpp>

#include <core/vulkan/Check.hpp>
#include <core/vulkan/internal/VMA.hpp>

namespace core {

CORE_VK_RESOURCE_DESTROY_IMPL(RawImage) {
    if (allocation.allocator != VMA_NULL && allocation.allocation != VMA_NULL) {
        vmaDestroyImage(allocation.allocator, self.m_handle, allocation.allocation);
    }
}

ImageView RawImage::createView(Device const& device, ImageView::Info const& info) {
    VkImageViewCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = m_handle,
        .viewType = static_cast<VkImageViewType>(info.type),
        .format = static_cast<VkFormat>(m_info.format),
        .components = VkComponentMapping{
            .r = static_cast<VkComponentSwizzle>(info.mapping.r),
            .g = static_cast<VkComponentSwizzle>(info.mapping.g),
            .b = static_cast<VkComponentSwizzle>(info.mapping.b),
            .a = static_cast<VkComponentSwizzle>(info.mapping.a),
        },
        .subresourceRange = VkImageSubresourceRange{
            .aspectMask = info.aspect.value,
            .baseMipLevel = info.base_mip_level,
            .levelCount = info.level_count,
            .baseArrayLayer = info.base_array_layer,
            .layerCount = info.layer_count,
        },
    };
    VkImageView result = VK_NULL_HANDLE;
    if (!VK_CHECK(vkCreateImageView(device.handle(), &create_info, nullptr, &result))) {
        throw ImageViewCreationError();
    }
    return ImageView(result, info, device);
}

} // namespace core
