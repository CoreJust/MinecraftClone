#include "Version.hpp"
#include <tuple>
#include <Core/Common/Assert.hpp>
#include "Internal/Vulkan/Functions.hpp"

namespace graphics::vulkan {
    Version g_vkVersion { };
    Version g_deviceVkVersion { };
    
    Version Version::fromVk(u32 vkVersion) noexcept {
        return {{
            .variant = VK_API_VERSION_VARIANT(vkVersion),
            .major   = VK_API_VERSION_MAJOR  (vkVersion),
            .minor   = VK_API_VERSION_MINOR  (vkVersion),
            .patch   = VK_API_VERSION_PATCH  (vkVersion),
        }};
    }

    u32 Version::asVk() const noexcept {
        return VK_MAKE_API_VERSION(variant, major, minor, patch);
    }

    void loadVkVersion() {
        uint32_t apiVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        if (internal::pvkEnumerateInstanceVersion)
            internal::pvkEnumerateInstanceVersion(&apiVersion);
        g_vkVersion = Version::fromVk(apiVersion);
    }

    void setDeviceVkVersion(Version const& version) noexcept {
        g_deviceVkVersion = version;
    }

    Version const& getVkVersion() noexcept {
        return g_vkVersion;
    }

    Version const& getDeviceVkVersion() noexcept {
        return g_deviceVkVersion;
    }

    u32 getVkVersionMajor() noexcept {
        return g_vkVersion.major;
    }
    
    u32 getVkVersionMinor() noexcept {
        return g_vkVersion.minor;
    }
    
    u32 getVkVersionPatch() noexcept {
        return g_vkVersion.patch;
    }
} // namespace graphics::vulkan
