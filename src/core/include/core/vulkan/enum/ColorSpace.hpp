#pragma once

#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

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

} // namespace core::vk

CORE_VK_REGISTER_ENUM(ColorSpace, { DisplayP3Nonlinear, 1'000'104'001 });
