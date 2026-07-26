#pragma once

#include <core/common/EnumBits.hpp>
#include <core/vulkan/enum/VulkanEnum.hpp>

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

CORE_VK_REGISTER_ENUM(CommandPoolFlag);
