#pragma once

#include <core/common/EnumBits.hpp>
#include <core/meta/Enum.hpp>

#include <cstdint>

namespace core {

enum class SurfaceTransform {
    Identity,
    Rotate90,
    Rotate180,
    Rotate270,
    HorizontalMirror,
    HorizontalMirrorRotate90,
    HorizontalMirrorRotate180,
    HorizontalMirrorRotate270,
    Inherit,

    Count,
};

CORE_ENUM_FUNCTIONS(SurfaceTransform);

using SurfaceTransformBits = EnumBits<SurfaceTransform>;

} // namespace core
