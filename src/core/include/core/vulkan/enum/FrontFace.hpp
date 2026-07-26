#pragma once

#include <core/meta/Enum.hpp>

namespace core::vk {

enum class FrontFace {
    CounterClockwise,
    Clockwise,

    Count,
};

} // namespace core::vk

CORE_ENUM_FUNCTIONS(vk::FrontFace);
