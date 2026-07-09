#pragma once

#include <core/common/EnumBits.hpp>
#include <core/meta/Enum.hpp>

namespace core::vk {

enum class CommandBufferUsage {
    OneTimeSubmit,
    RenderPassContinue,
    SimultaneousUse,

    Count,
};

using CommandBufferUsageBits = EnumBits<CommandBufferUsage>;

} // namespace core::vk

CORE_ENUM_FUNCTIONS(vk::CommandBufferUsage);
