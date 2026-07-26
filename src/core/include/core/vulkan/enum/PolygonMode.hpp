#pragma once

#include <core/meta/Enum.hpp>

namespace core::vk {

enum class PolygonMode {
    Fill,
    Line,
    Point,

    Count,
};

} // namespace core::vk

CORE_ENUM_FUNCTIONS(::core::vk::PolygonMode);
