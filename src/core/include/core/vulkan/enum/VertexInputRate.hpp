#pragma once

#include <core/meta/Enum.hpp>

namespace core::vk {

enum class VertexInputRate {
    Vertex,
    Instance,

    Count,
};

} // namespace core::vk

CORE_ENUM_FUNCTIONS(vk::VertexInputRate);
