#pragma once

#include <core/meta/Enum.hpp>

namespace core::vk {

enum class CompareOp {
    Never,
    Less,
    Eq,
    LessEq,
    Greater,
    NotEq,
    GreaterEq,
    Always,

    Count,
};

} // namespace core::vk

CORE_ENUM_FUNCTIONS(::core::vk::CompareOp);
