#pragma once

#include <core/meta/Enum.hpp>

namespace core::vk {

enum class StencilOp {
    Keep,
    Zero,
    Replace,
    IncClamp, // Increment then clamp
    DecClamp, // Decrement then clamp
    Invert,
    IncWrap, // Increment then wrap
    DecWrap, // Decrement then wrap

    Count,
};

} // namespace core::vk

CORE_ENUM_FUNCTIONS(vk::StencilOp);
