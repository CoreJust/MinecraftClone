#pragma once

#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

enum class ComponentSwizzle {
    Identity,
    Zero,
    One,
    R,
    G,
    B,
    A,

    Count,
};

struct ComponentMapping final {
    ComponentSwizzle r;
    ComponentSwizzle g;
    ComponentSwizzle b;
    ComponentSwizzle a;
};

} // namespace core::vk

CORE_VK_REGISTER_ENUM(ComponentSwizzle);
