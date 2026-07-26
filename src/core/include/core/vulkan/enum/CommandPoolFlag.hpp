#pragma once

#include <core/common/EnumBits.hpp>
#include <core/meta/Enum.hpp>

#include <cstdint>

namespace core::vk {

enum class CommandPoolFlag {
    Transient,
    ResetCommandBuffer,
    Protected,

    Count,
};

using CommandPoolFlags = EnumBits<CommandPoolFlag>;

} // namespace core::vk

CORE_ENUM_FUNCTIONS(::core::vk::CommandPoolFlag);
