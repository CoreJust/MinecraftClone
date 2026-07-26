#pragma once

#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

enum class CommandBufferType {
    Primary,
    Secondary,

    Count,
};

} // namespace core::vk

CORE_VK_REGISTER_ENUM(CommandBufferType);
