#pragma once

#include <core/common/EnumBits.hpp>
#include <core/meta/Enum.hpp>

#include <cstdint>

namespace core {

enum class CommandPoolFlag {
    Transient,
    ResetCommandBuffer,
    Protected,

    Count,
};

CORE_ENUM_FUNCTIONS(CommandPoolFlag);

using CommandPoolFlags = EnumBits<CommandPoolFlag>;

} // namespace core
