#pragma once
#include <cstdint>
#include <Core/Macro/Attributes.hpp>

namespace graphics::vulkan {
    struct Version final {
        uint32_t variant = 0;
        uint32_t major   = 1;
        uint32_t minor   = 0;
        uint32_t patch   = 0;

        PURE bool operator<(Version const& lhs) const noexcept;

        PURE static Version fromVk(uint32_t vkVersion) noexcept;
        PURE uint32_t asVk() const noexcept;
    };

    void loadVkVersion();
    void downgradeVkVersion(Version newVersion);

    PURE Version const& getVkVersion();
    PURE uint32_t getVkVersionMajor();
    PURE uint32_t getVkVersionMinor();
    PURE uint32_t getVkVersionPatch();
} // namespace graphics::vulkan
