#include "Version.hpp"
#include <tuple>
#include <Core/Common/Assert.hpp>
#include "Internal/Vulkan/Functions.hpp"
#include "Internal/Vulkan/Instance.hpp"
#include "Internal/Vulkan/PhysicalDevice.hpp"

namespace graphics::vulkan {
    Version g_vkVersion {};
    
    Version Version::fromVk(uint32_t vkVersion) noexcept {
        return {{
            .variant = VK_API_VERSION_VARIANT(vkVersion),
            .major   = VK_API_VERSION_MAJOR  (vkVersion),
            .minor   = VK_API_VERSION_MINOR  (vkVersion),
            .patch   = VK_API_VERSION_PATCH  (vkVersion),
        }};
    }

    uint32_t Version::asVk() const noexcept {
        return VK_MAKE_API_VERSION(variant, major, minor, patch);
    }

    void loadVkVersion() {
        internal::Instance instance = internal::Instance::makeTemporaryInstance();
        g_vkVersion = internal::PhysicalDevice::getHighestPhysicalDeviceVersion(instance);
    }

    Version getMaxInstanceVersion() {
        uint32_t apiVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        if (internal::pvkEnumerateInstanceVersion)
            internal::pvkEnumerateInstanceVersion(&apiVersion);
        return Version::fromVk(apiVersion);
    }

    Version const& getVkVersion() {
        return g_vkVersion;
    }

    uint32_t getVkVersionMajor() {
        return g_vkVersion.major;
    }
    
    uint32_t getVkVersionMinor() {
        return g_vkVersion.minor;
    }
    
    uint32_t getVkVersionPatch() {
        return g_vkVersion.patch;
    }
} // namespace graphics::vulkan
