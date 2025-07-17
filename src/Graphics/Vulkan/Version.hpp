#pragma once
#include <Core/Common/Version.hpp>

namespace graphics::vulkan {
    struct Version final : public core::Version {
        PURE bool operator<(Version const& lhs) const noexcept { return core::Version::operator<(lhs); }

        PURE static Version fromVk(u32 vkVersion) noexcept;
        PURE u32 asVk() const noexcept;
    };

    void loadVkVersion();
    void setDeviceVkVersion(Version const& version) noexcept;

    PURE Version const& getVkVersion() noexcept;
    PURE Version const& getDeviceVkVersion() noexcept;
    PURE u32 getVkVersionMajor() noexcept;
    PURE u32 getVkVersionMinor() noexcept;
    PURE u32 getVkVersionPatch() noexcept;
} // namespace graphics::vulkan
