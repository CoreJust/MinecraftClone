#pragma once

#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

enum class VertexInputRate {
    Vertex,
    Instance,

    Count,
};

} // namespace core::vk

CORE_VK_REGISTER_ENUM(VertexInputRate);
