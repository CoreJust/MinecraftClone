#pragma once

#include <core/common/EnumBits.hpp>
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

using MemoryPropertyBits = EnumBits<MemoryProperty>;

struct MemoryHeap final {
    MemoryPropertyBits properties;
    uint32_t heap_index;
};

} // namespace core
