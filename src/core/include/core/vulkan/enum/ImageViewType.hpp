#pragma once

#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

enum class ImageViewType {
    OneD,
    TwoD,
    ThreeD,
    Cube,
    OneDArray,
    TwoDArray,
    CubeArray,

    Count,
};

} // namespace core::vk

CORE_VK_REGISTER_ENUM(ImageViewType);
