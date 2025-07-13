#pragma once
#include <Core/Common/Version.hpp>

namespace graphics::vulkan {
    struct Version final : public core::common::Version {
        PURE bool operator<(Version const& lhs) const noexcept { return core::common::Version::operator<(lhs); }

        PURE static Version fromVk(uint32_t vkVersion) noexcept;
        PURE uint32_t asVk() const noexcept;
    };

    void loadVkVersion();
    
    PURE Version getMaxInstanceVersion(); 

    PURE Version const& getVkVersion();
    PURE uint32_t getVkVersionMajor();
    PURE uint32_t getVkVersionMinor();
    PURE uint32_t getVkVersionPatch();
} // namespace graphics::vulkan
