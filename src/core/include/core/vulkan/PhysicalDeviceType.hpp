#pragma once

#include <core/meta/Enum.hpp>

namespace core {

// Might be static_cast'ed to VkPhysicalDeviceType
enum class PhysicalDeviceType {
    Other,
    Integrated,
    Discrete,
    Virtual,
    Cpu,

    Count,
};

CORE_ENUM_FUNCTIONS(PhysicalDeviceType);

} // namespace core
