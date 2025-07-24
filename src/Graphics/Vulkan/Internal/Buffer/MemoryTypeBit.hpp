#pragma once

namespace graphics::vulkan::internal {
    enum class MemoryTypeBit {
        None = 0,

        // Specifies that memory allocated with this type is the most efficient for device access.
        // This property will be set if and only if the memory type belongs to a heap with the 
        // VK_MEMORY_HEAP_DEVICE_LOCAL_BIT set.
        DeviceLocal = 1,

        // Specifies that memory allocated with this type can be mapped for host access using vkMapMemory.
        HostVisible = 2,

        // Specifies that the host cache management commands vkFlushMappedMemoryRanges and 
        // vkInvalidateMappedMemoryRanges are not needed to manage availability and visibility on the host.
        HostCoherent = 4,

        // Specifies that memory allocated with this type is cached on the host. Host memory accesses 
        // to uncached memory are slower than to cached memory, however uncached memory is always host 
        // coherent.
        HostCached = 8,

        // Specifies that the memory type only allows device access to the memory. Memory types must not have 
        // both VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT and VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT set.
        // Additionally, the object’s backing memory may be provided by the implementation lazily as specified 
        // in Lazily Allocated Memory.
        LazilyAllocated = 16,

        // Since Vulkan 1.1
        // Specifies that the memory type only allows device access to the memory, and allows protected queue
        // operations to access the memory. Memory types must not have VK_MEMORY_PROPERTY_PROTECTED_BIT set 
        // and any of VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT set, or VK_MEMORY_PROPERTY_HOST_COHERENT_BIT set, 
        // or VK_MEMORY_PROPERTY_HOST_CACHED_BIT set.
        Protected = 32,
    };

    constexpr MemoryTypeBit operator|(MemoryTypeBit lhs, MemoryTypeBit rhs) noexcept {
        return static_cast<MemoryTypeBit>(static_cast<unsigned int>(lhs) | static_cast<unsigned int>(rhs));
    }

    constexpr MemoryTypeBit operator&(MemoryTypeBit lhs, MemoryTypeBit rhs) noexcept {
        return static_cast<MemoryTypeBit>(static_cast<unsigned int>(lhs) & static_cast<unsigned int>(rhs));
    }
} // namespace graphics::vulkan::internal
