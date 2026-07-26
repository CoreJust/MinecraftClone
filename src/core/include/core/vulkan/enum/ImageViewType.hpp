#pragma once

#include <core/meta/Enum.hpp>

namespace core::vk {

enum class ImageViewType {
    OneD,
    TwoD,
    ThreeD,
    Cube,
    OneDArray,
    TwoDArray,
    CubeArray,

    Count,
};

} // namespace core::vk

CORE_ENUM_FUNCTIONS(::core::vk::ImageViewType);
