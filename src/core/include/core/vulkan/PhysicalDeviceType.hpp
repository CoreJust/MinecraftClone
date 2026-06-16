#pragma once

#include <string>

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

[[nodiscard]]
inline std::string to_string(PhysicalDeviceType const type) {
    switch(type) {
        case PhysicalDeviceType::Other:      return "Other";
        case PhysicalDeviceType::Integrated: return "Integrated";
        case PhysicalDeviceType::Discrete:   return "Discrete";
        case PhysicalDeviceType::Virtual:    return "Virtual";
        case PhysicalDeviceType::Cpu:        return "Cpu";
    case PhysicalDeviceType::Count: return "Count";
    }
}

} // namespace core
