#pragma once

#include <core/common/EnumBits.hpp>
#include <core/meta/Enum.hpp>

namespace core {

enum class CommandBufferUsage {
    OneTimeSubmit,
    RenderPassContinue,
    SimultaneousUse,

    Count,
};

CORE_ENUM_FUNCTIONS(CommandBufferUsage);

using CommandBufferUsageBits = EnumBits<CommandBufferUsage>;

} // namespace core
