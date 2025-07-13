#pragma once
#include <cstdint>
#include <Core/Macro/Attributes.hpp>

namespace core::common {
    struct Version {
        uint32_t variant = 0;
        uint32_t major   = 1;
        uint32_t minor   = 0;
        uint32_t patch   = 0;

        PURE bool operator<(Version const& lhs) const noexcept;
    };
} // namespace core::common
