#pragma once

#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

enum class CompareOp {
    Never,
    Less,
    Eq,
    LessEq,
    Greater,
    NotEq,
    GreaterEq,
    Always,

    Count,
};

} // namespace core::vk

CORE_VK_REGISTER_ENUM(CompareOp);
