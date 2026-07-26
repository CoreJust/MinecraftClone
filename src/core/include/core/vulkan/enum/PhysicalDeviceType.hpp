#pragma once

#include <core/vulkan/enum/VulkanEnum.hpp>

namespace core::vk {

// Might be static_cast'ed to VkPhysicalDeviceType
enum class PhysicalDeviceType {
    Other,
    Integrated,
    Discrete,
    Virtual,
    Cpu,

    Count,
};

} // namespace core::vk

CORE_VK_REGISTER_ENUM(PhysicalDeviceType);
