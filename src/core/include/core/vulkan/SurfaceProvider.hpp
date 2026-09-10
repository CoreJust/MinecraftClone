#pragma once

#include <core/vulkan/Extensions.hpp>
#include <core/vulkan/Extent.hpp>

// DONT_CHECK INCLUDE_ORDER
#include <volk.h>

#include <span>

namespace core::vk {

class SurfaceProvider {
public:
    virtual ~SurfaceProvider() = default;

    [[nodiscard]]
    virtual std::span<VulkanExtension const> requiredInstanceExtensions() const noexcept = 0;
    [[nodiscard]]
    virtual Extent2d framebufferExtent() const noexcept = 0;
    [[nodiscard]]
    virtual bool isFramebufferExtentZero() const noexcept = 0;
    [[nodiscard]]
    virtual VkSurfaceKHR createSurface(VkInstance instance) const = 0;
};

} // namespace core::vk
