#pragma once

#include <core/meta/Enum.hpp>

namespace core {

enum class CommandBufferType {
    Primary,
    Secondary,

    Count,
};

CORE_ENUM_FUNCTIONS(CommandBufferType);

} // namespace core
