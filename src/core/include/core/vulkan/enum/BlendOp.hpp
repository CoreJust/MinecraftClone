#pragma once

#include <core/meta/Enum.hpp>

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

// TODO: add VulkanEnum.hpp with helpers, including general toVk<T> and fromVk<T>.
[[nodiscard]]
constexpr BlendOp blendOpFromVk(uint32_t const op) noexcept {
    return static_cast<BlendOp>(op >= 1000148000
        ? op - 1000148000 + static_cast<uint32_t>(BlendOp::Zero)
        : op
    );
}

[[nodiscard]]
constexpr uint32_t blendOpToVk(BlendOp const op) noexcept {
    uint32_t const as_uint32 = static_cast<uint32_t>(op);
    return as_uint32 >= static_cast<uint32_t>(BlendOp::Zero)
        ? as_uint32 + 1000148000 - static_cast<uint32_t>(BlendOp::Zero)
        : as_uint32
    ;
}

} // namespace core::vk

CORE_ENUM_FUNCTIONS(vk::BlendOp);
