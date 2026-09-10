#pragma once

#include <core/common/EnumBits.hpp>
#include <core/vulkan/enum/VulkanEnum.hpp>

#include <cstdint>

namespace core::vk {

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

using MemoryPropertyBits = EnumBits<MemoryProperty>;

struct MemoryHeap final {
    MemoryPropertyBits properties;
    uint32_t heap_index;
};

} // namespace core::vk

CORE_VK_REGISTER_ENUM(MemoryProperty);
