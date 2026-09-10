#pragma once

#include <core/vulkan/Allocation.hpp>
#include <core/vulkan/Device.hpp>
#include <core/vulkan/Error.hpp>
#include <core/vulkan/Extent.hpp>
#include <core/vulkan/ImageView.hpp>
#include <core/vulkan/Resource.hpp>
#include <core/vulkan/enum/Format.hpp>
#include <core/vulkan/enum/ImageUsage.hpp>

namespace core::vk {

struct ImageViewCreationError final : public VulkanError {
    ImageViewCreationError() : VulkanError("Failed to create ImageView from Image") { }
};

class RawImage : public VulkanResourceBase<VkImage> {
public:
    struct Info final {
        Extent3d extent;
        Format format;
        ImageUsage usage;
    };
private:
    CORE_VK_RESOURCE_CONTEXT(RawImage,
        Allocation allocation;);
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(VkImage const image, Info const& info, Allocation const allocation = { }) {
        CORE_VK_CAPTURE_DESTRUCTION_CONTEXT(){ .allocation = allocation };
        self.m_handle = image;
        self.m_info = info;
    }
public:
    // Throws ImageViewCreationError
    [[nodiscard]]
    ImageView createView(Device const& device, ImageView::Info const& info = {});

    [[nodiscard]]
    constexpr Info const& info() const noexcept { return m_info; }
    [[nodiscard]]
    constexpr Extent3d const& extent() const noexcept { return m_info.extent; }
    [[nodiscard]]
    constexpr uint32_t width() const noexcept { return m_info.extent.x; }
    [[nodiscard]]
    constexpr uint32_t height() const noexcept { return m_info.extent.y; }
    [[nodiscard]]
    constexpr uint32_t depth() const noexcept { return m_info.extent.z; }
    [[nodiscard]]
    constexpr Format format() const noexcept { return m_info.format; }
    [[nodiscard]]
    constexpr ImageUsage usage() const noexcept { return m_info.usage; }
private:
    Info m_info;
};

using Image = VulkanRaii<RawImage>;

} // namespace core::vk
