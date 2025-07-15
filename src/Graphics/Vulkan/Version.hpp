#pragma once
#include <Core/Common/Version.hpp>

namespace graphics::vulkan {
    struct Version final : public core::Version {
        PURE bool operator<(Version const& lhs) const noexcept { return core::Version::operator<(lhs); }

        PURE static Version fromVk(u32 vkVersion) noexcept;
        PURE u32 asVk() const noexcept;
    };

    void loadVkVersion();
    
    PURE Version getMaxInstanceVersion(); 

    PURE Version const& getVkVersion();
    PURE u32 getVkVersionMajor();
    PURE u32 getVkVersionMinor();
    PURE u32 getVkVersionPatch();
} // namespace graphics::vulkan
