#pragma once

#include <core/common/EnumBits.hpp>
#include <core/meta/Enum.hpp>

#include <cstdint>

namespace core::vk {

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

using SurfaceTransformBits = EnumBits<SurfaceTransform>;

} // namespace core::vk

CORE_ENUM_FUNCTIONS(vk::SurfaceTransform);
