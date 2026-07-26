#pragma once

#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

enum class PolygonMode {
    Fill,
    Line,
    Point,

    Count,
};

} // namespace core::vk

CORE_VK_REGISTER_ENUM(PolygonMode);
