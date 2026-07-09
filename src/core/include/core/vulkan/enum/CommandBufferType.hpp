#pragma once

#include <core/meta/Enum.hpp>

namespace core::vk {

enum class CommandBufferType {
    Primary,
    Secondary,

    Count,
};

} // namespace core::vk

CORE_ENUM_FUNCTIONS(vk::CommandBufferType);
