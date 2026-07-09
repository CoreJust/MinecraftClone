#pragma once

#include <core/meta/Enum.hpp>

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

CORE_ENUM_FUNCTIONS(vk::PhysicalDeviceType);
