#pragma once

#include <core/vulkan/Device.hpp>
#include <core/vulkan/Resource.hpp>
#include <core/vulkan/enum/ComponentMapping.hpp>
#include <core/vulkan/enum/ImageAspect.hpp>
#include <core/vulkan/enum/ImageViewType.hpp>

namespace core::vk {

class RawImageView : public VulkanResourceBase<VkImageView> {
public:
    struct Info final {
        ImageViewType type = ImageViewType::TwoD;
        ComponentMapping mapping = ComponentMapping {
            .r = ComponentSwizzle::Identity,
            .g = ComponentSwizzle::Identity,
            .b = ComponentSwizzle::Identity,
            .a = ComponentSwizzle::Identity,
        };
        ImageAspectBits aspect = ImageAspectBits::of(ImageAspect::Color);
        uint32_t base_mip_level = 0;
        uint32_t level_count = 1;
        uint32_t base_array_layer = 0;
        uint32_t layer_count = 1;
    };
private:
    CORE_VK_RESOURCE_CONTEXT(RawImageView,
        RawDevice device;);
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(VkImageView const image_view, Info const& info, Device const& device) {
        CORE_VK_CAPTURE_DESTRUCTION_CONTEXT(){ .device = device.raw() };
        self.m_handle = image_view;
        self.m_info = info;
    }
public:
    [[nodiscard]]
    constexpr Info const& info() const noexcept { return m_info; }
private:
    Info m_info;
};

using ImageView = VulkanRaii<RawImageView>;

} // namespace core::vk
