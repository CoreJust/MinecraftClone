#pragma once

#include <core/common/SpanUtils.hpp>

#include <cstdint>
#include <string>

namespace core {

enum class MemoryProperty {
    DeviceLocal,
    HostVisible,
    HostCoherent,
    HostCached,
    LazilyAllocated,
    Protected,
    DeviceCoherent,
    DeviceUncached,
    RdmaCapable,

    Count,
};

struct MemoryPropertyBits final {
    uint32_t value;

    template<typename... Args> [[nodiscard]]
    static constexpr MemoryPropertyBits of(MemoryProperty const first, Args const... args) noexcept {
        if constexpr (sizeof...(Args) > 0) {
            return { 1u << static_cast<uint32_t>(first) | memoryPropertyFlagBitsOf(args...).value };
        } else {
            return { 1u << static_cast<uint32_t>(first) };
        }
    }
};

struct MemoryHeap final {
    MemoryPropertyBits properties;
    uint32_t heap_index;
};

[[nodiscard]]
inline std::string to_string(MemoryProperty const property) {
    switch (property) {
        case MemoryProperty::DeviceLocal:     return "DeviceLocal";
        case MemoryProperty::HostVisible:     return "HostVisible";
        case MemoryProperty::HostCoherent:    return "HostCoherent";
        case MemoryProperty::HostCached:      return "HostCached";
        case MemoryProperty::LazilyAllocated: return "LazilyAllocated";
        case MemoryProperty::Protected:       return "Protected";
        case MemoryProperty::DeviceCoherent:  return "DeviceCoherent";
        case MemoryProperty::DeviceUncached:  return "DeviceUncached";
        case MemoryProperty::RdmaCapable:     return "RdmaCapable";
    case MemoryProperty::Count: return "Count";
    }
}

} // namespace core
