#include <core/vulkan/VulkanVersion.hpp>

#include <volk.h>

namespace core::vk {

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

} // namespace core::vk
