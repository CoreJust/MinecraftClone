#pragma once

#include <core/common/EnumBits.hpp>
#include <core/meta/Enum.hpp>

namespace core {

enum class ImageAspect {
    Color,
    Depth,
    Stencil,
    Metadata,
    Plain0,
    Plain1,
    Plain2,
    MemoryPlain0,
    MemoryPlain1,
    MemoryPlain2,
    MemoryPlain3,

    Count,
};

CORE_ENUM_FUNCTIONS(ImageAspect);

using ImageAspectBits = EnumBits<ImageAspect>;

} // namespace core
