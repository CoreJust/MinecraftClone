#include "AndroidSurfaceProvider.hpp"

#include <core/common/Assert.hpp>
#include <core/vulkan/Check.hpp>

#include <android/native_window.h>

#include <array>

namespace game_android {
namespace {

constexpr std::array REQUIRED_INSTANCE_EXTENSIONS{
    core::vk::VulkanExtension::Surface,
    core::vk::VulkanExtension::AndroidSurface,
};

} // namespace

AndroidSurfaceProvider::AndroidSurfaceProvider(ANativeWindow* const window) noexcept
    : m_window(window)
{
    ASSERT(m_window != nullptr, "Android surface provider requires a native window");
}

std::span<core::vk::VulkanExtension const> AndroidSurfaceProvider::requiredInstanceExtensions() const noexcept
{
    return REQUIRED_INSTANCE_EXTENSIONS;
}

core::vk::Extent2d AndroidSurfaceProvider::framebufferExtent() const noexcept
{
    ASSERT(m_window != nullptr, "Android surface provider has no native window");
    int32_t const width = ANativeWindow_getWidth(m_window);
    int32_t const height = ANativeWindow_getHeight(m_window);
    return {
        static_cast<uint32_t>(width > 0 ? width : 1),
        static_cast<uint32_t>(height > 0 ? height : 1),
    };
}

bool AndroidSurfaceProvider::isFramebufferExtentZero() const noexcept
{
    ASSERT(m_window != nullptr, "Android surface provider has no native window");
    return ANativeWindow_getWidth(m_window) <= 0 || ANativeWindow_getHeight(m_window) <= 0;
}

VkSurfaceKHR AndroidSurfaceProvider::createSurface(VkInstance const instance) const
{
    ASSERT(m_window != nullptr, "Android surface provider has no native window");
    VkAndroidSurfaceCreateInfoKHR const info{
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .window = m_window,
    };
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    CORE_VK_ASSERT(vkCreateAndroidSurfaceKHR(instance, &info, nullptr, &surface));
    return surface;
}

} // namespace game_android
