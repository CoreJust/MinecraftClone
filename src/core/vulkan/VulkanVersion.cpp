#include <core/vulkan/VulkanVersion.hpp>

#include <vulkan.h>

namespace core {
namespace {
Version g_vk_version { };
Version g_device_vk_version { };
} // namespace

Version vkToVersion(uint32_t const vk_version) noexcept {
    return {
        .epoch = VK_API_VERSION_VARIANT(vk_version),
        .major = VK_API_VERSION_MAJOR  (vk_version),
        .minor = VK_API_VERSION_MINOR  (vk_version),
        .patch = VK_API_VERSION_PATCH  (vk_version),
    };
}

uint32_t versionToVk(Version const& version) noexcept {
    return VK_MAKE_API_VERSION(version.epoch, version.major, version.minor, version.patch);
}

void setVkVersion(Version const& version) noexcept {
    g_vk_version = version;
}

void setDeviceVkVersion(Version const& version) noexcept {
    g_device_vk_version = version;
}

Version const& getVkVersion() noexcept {
    return g_vk_version;
}

Version const& getDeviceVkVersion() noexcept {
    return g_device_vk_version;
}

} // namespace core
