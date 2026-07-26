#pragma once

#include <core/meta/Enum.hpp>

namespace core::vk {

enum class CullMode {
    None,
    Front,
    Back,
    All,

    Count,
};

} // namespace core::vk

CORE_ENUM_FUNCTIONS(vk::CullMode);
