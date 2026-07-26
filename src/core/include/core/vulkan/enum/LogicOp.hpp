#pragma once

#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

enum class LogicOp {
    Clear,
    And,
    AndReverse,
    Copy,
    AndInnverted,
    Nop,
    Xor,
    Or,
    Nor,
    Equivalent,
    Invert,
    OrReverse,
    CopyInverted,
    OrInverted,
    Nand,
    Set,

    Count,
};

} // namespace core::vk

CORE_VK_REGISTER_ENUM(LogicOp);
