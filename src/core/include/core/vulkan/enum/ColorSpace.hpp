#pragma once

#include <core/meta/Enum.hpp>

namespace core {

enum class ColorSpace {
    SRGBNonlinear,
    DisplayP3Nonlinear,
    ExtendedSRGBLinear,
    DisplayP3Linear,
    DCIP3Nonlinear,
    BT709Linear,
    BT709Nonlinear,
    BT2020Linear,
    HDR10ST2084,
    Dolbyvision,
    HDR10HLG,
    AdobeRGBLinear,
    AdobeRGBNonlinear,
    PassThrough,
    ExtendedSRGBNonlinear,

    Count,
};

CORE_ENUM_FUNCTIONS(ColorSpace);

[[nodiscard]]
constexpr uint32_t colorSpaceToVk(ColorSpace const color_space) noexcept {
    return color_space == ColorSpace::SRGBNonlinear
        ? 0
        : static_cast<uint32_t>(color_space) + 1'000'104'000;
}

[[nodiscard]]
constexpr ColorSpace vkToColorSpace(uint32_t const color_space) noexcept {
    return color_space == 0
        ? ColorSpace::SRGBNonlinear
        : static_cast<ColorSpace>(color_space - 1'000'104'000);
}

} // namespace core
