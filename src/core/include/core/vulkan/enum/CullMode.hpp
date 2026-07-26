#pragma once

#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

enum class CullMode {
    None,
    Front,
    Back,
    All,

    Count,
};

} // namespace core::vk

CORE_VK_REGISTER_ENUM(CullMode);
