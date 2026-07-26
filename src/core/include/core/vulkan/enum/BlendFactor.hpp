#pragma once

#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

enum class BlendFactor {
    Zero,
    One,
    SrcColor,
    OneMinusSrcColor,
    DstColor,
    OneMinusDstColor,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
    ConstColor,
    OneMinusConstColor,
    ConstAlpha,
    OneMinusConstAlpha,
    SrcAlphaSaturate,
    Src1Color,
    OneMinusSrc1Color,
    Src1Alpha,
    OneMinusSrc1Alpha,

    Count,
};

} // namespace core::vk

CORE_VK_REGISTER_ENUM(BlendFactor);
