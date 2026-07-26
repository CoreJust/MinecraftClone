#pragma once

#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

enum class BlendOp {
    Add,
    Subtract,
    ReverseSubtract,
    Min,
    Max,
    Zero,
    Src,
    Dst,
    SrcOver,
    DstOver,
    SrcIn,
    DstIn,
    SrcOut,
    DstOut,
    SrcAtop,
    DstAtop,
    Xor,
    Multiply,
    Screen,
    Overlay,
    Darken,
    Lighten,
    ColorDodge,
    ColorBurn,
    HardLight,
    SoftLight,
    Diff,
    Exclusion,
    Invert,
    InvertRGB,
    LinearDodge,
    LinearBurn,
    VividLight,
    LinearLight,
    PinLight,
    HardMix,
    HSLHue,
    HSLSaturation,
    HSLColor,
    HSLLuminosity,
    Plus,
    PlusClamped,
    PlusClampedAlpha,
    PlusDarker,
    Minus,
    MinusClamped,
    Contrast,
    InvertOVG,
    R,
    G,
    B,

    Count,
};

} // namespace core::vk

CORE_VK_REGISTER_ENUM(BlendOp, { Zero, 1'000'148'000 });
