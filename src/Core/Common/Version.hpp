#pragma once
#include <Core/Macro/Attributes.hpp>
#include "Int.hpp"

namespace core::common {
    struct Version {
        u32 variant = 0;
        u32 major   = 1;
        u32 minor   = 0;
        u32 patch   = 0;

        PURE bool operator<(Version const& lhs) const noexcept;
    };
} // namespace core::common
