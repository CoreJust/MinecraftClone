#pragma once

#include <core/meta/Enum.hpp>

namespace core::vk {

enum class PrimitiveTopology {
    PointList,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip,
    TriangleFan,
    LineListWithAdjacency,
    LineStripWithAdjacency,
    TriangleListWithAdjacency,
    TriangleStripWithAdjacency,
    PatchList,

    Count,
};

} // namespace core::vk

CORE_ENUM_FUNCTIONS(::core::vk::PrimitiveTopology);
