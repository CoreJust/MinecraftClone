#pragma once

#include <core/meta/Enum.hpp>

namespace core {

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

CORE_ENUM_FUNCTIONS(ImageViewType);

} // namespace core
