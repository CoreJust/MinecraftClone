#pragma once

#include <core/common/EnumBits.hpp>
#include <core/meta/Enum.hpp>

namespace core::vk {

enum class ColorComponent {
    R,
    G,
    B,
    A,

    Count,
};

using ColorComponents = EnumBits<ColorComponent>;

} // namespace core::vk

CORE_ENUM_FUNCTIONS(vk::ColorComponent);
