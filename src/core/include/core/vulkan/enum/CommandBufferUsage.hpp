#pragma once

#include <core/common/EnumBits.hpp>
#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

enum class CommandBufferUsage {
    OneTimeSubmit,
    RenderPassContinue,
    SimultaneousUse,

    Count,
};

using CommandBufferUsageBits = EnumBits<CommandBufferUsage>;

} // namespace core::vk

CORE_VK_REGISTER_ENUM(CommandBufferUsage);
