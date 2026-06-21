#pragma once

#include <core/vulkan/Allocation.hpp>
#include <core/vulkan/Extent.hpp>
#include <core/vulkan/Resource.hpp>

namespace core {

class RawImage : public VulkanResourceBase<VkImage> {
public:
    struct Info final {
        Extent3d extent;
        // TODO: fill
    };
private:
    CORE_VK_RESOURCE_CONTEXT(RawImage,
        Allocation allocation;);
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(VkImage const image, Info const info, Allocation const allocation = { }) {
        CORE_VK_CAPTURE_DESTRUCTION_CONTEXT(){ .allocation = allocation };
        self.m_handle = image;
        self.m_info = info;
    }
public:
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
private:
    Info m_info;
};

using Image = VulkanRaii<RawImage>;

} // namespace core
