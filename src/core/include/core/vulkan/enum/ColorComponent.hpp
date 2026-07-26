#pragma once

#include <core/common/EnumBits.hpp>
#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

enum class ColorComponent {
    R,
    G,
    B,
    A,

    Count,
};

using ColorComponents = EnumBits<ColorComponent>;

} // namespace core::vk

CORE_VK_REGISTER_ENUM(ColorComponent);
