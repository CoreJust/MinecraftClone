#pragma once

#include <core/meta/Enum.hpp>

namespace core::vk {

enum class LogicOp {
    Clear,
    And,
    AndReverse,
    Copy,
    AndInnverted,
    Nop,
    Xor,
    Or,
    Nor,
    Equivalent,
    Invert,
    OrReverse,
    CopyInverted,
    OrInverted,
    Nand,
    Set,

    Count,
};

} // namespace core::vk

CORE_ENUM_FUNCTIONS(vk::LogicOp);
