#pragma once

#include <core/common/SpanUtils.hpp>
#include <core/meta/Enum.hpp>

#include <cstdint>

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

CORE_ENUM_FUNCTIONS(MemoryProperty);

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

} // namespace core
